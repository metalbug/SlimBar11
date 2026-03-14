// ============================================================
// SlimBar11 Hook DLL 
// ============================================================

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dbghelp.h>
#include <psapi.h>
#include <detours.h>
#include <winhttp.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <aclapi.h>
#include <sddl.h>

#include <atomic>
#include <cstdio>
#include <cmath>
#include <limits>
#include <string>
#include <vector>
#include <mutex>
#include <set>
#include <condition_variable>

#undef GetCurrentTime 
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Media.h>

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "comctl32.lib")

static int TASKBAR_HEIGHT = 32;
static int ICON_SIZE      = 16;
static int TASKBAR_WIDTH_VERTICAL = 80;
static HMODULE g_hSelfModule = nullptr;

typedef HRESULT(WINAPI* PFN_GETDPI)(HMONITOR, int, UINT*, UINT*);
static PFN_GETDPI g_pGetDpiForMonitor = nullptr;

static bool ProtectMemcpy(void* dst, const void* src, size_t sz) {
    if (IsBadReadPtr(src, sz)) return false;
    DWORD old; 
    if (!VirtualProtect(dst, sz, PAGE_READWRITE, &old)) return false;
    memcpy(dst, src, sz); 
    VirtualProtect(dst, sz, old, &old); 
    return true;
}

static std::string GetModuleDir() { 
    char p[MAX_PATH]; 
    GetModuleFileNameA(g_hSelfModule, p, MAX_PATH); 
    char* s = strrchr(p, '\\'); 
    if (s) *s = 0; 
    return p; 
}

static std::mutex g_consoleMutex; 
static std::atomic<int> g_nextProgressLine{2}; 

static void PrintAt(int line, const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_consoleMutex);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!hOut || hOut == INVALID_HANDLE_VALUE) return;
    COORD pos = {0, (SHORT)line};
    SetConsoleCursorPosition(hOut, pos);
    
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    printf("%-110s", buf); 
}

static void OpenConsole() {
    if (AllocConsole()) {
        HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode;
        if (GetConsoleMode(hIn, &mode)) {
            mode &= ~ENABLE_QUICK_EDIT_MODE; 
            mode |= ENABLE_EXTENDED_FLAGS;
            SetConsoleMode(hIn, mode);
        }
        FILE* f;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
        printf("=== SlimBar11 Symbol Resolver ===\n\n");
    }
}

static void LoadConfig() {
    std::string iniPath = GetModuleDir() + "\\slimbar11-config.ini";
    TASKBAR_HEIGHT = GetPrivateProfileIntA("Settings", "TaskbarHeight", 32, iniPath.c_str());
    ICON_SIZE = GetPrivateProfileIntA("Settings", "IconSize", 16, iniPath.c_str());
    TASKBAR_WIDTH_VERTICAL = GetPrivateProfileIntA("Settings", "TaskbarWidthVertical", 80, iniPath.c_str());
}

static HWND FindTray() {
    HWND r = 0;
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        DWORD pid; 
        WCHAR cls[32];
        if (GetWindowThreadProcessId(h, &pid) && pid == GetCurrentProcessId() &&
            GetClassNameW(h, cls, 32) && _wcsicmp(cls, L"Shell_TrayWnd") == 0) { 
            *(HWND*)lp = h; 
            return FALSE; 
        }
        return TRUE;
    }, (LPARAM)&r);
    return r;
}

static void AllowAppContainerAccess() {
    char path[MAX_PATH];
    GetModuleFileNameA(g_hSelfModule, path, MAX_PATH);
    PACL pOldDACL = NULL, pNewDACL = NULL;
    PSECURITY_DESCRIPTOR pSD = NULL;
    if (GetNamedSecurityInfoA(path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &pOldDACL, NULL, &pSD) == ERROR_SUCCESS) {
        PSID pSID = NULL;
        if (ConvertStringSidToSidA("S-1-15-2-1", &pSID)) {
            EXPLICIT_ACCESS_A ea = {0};
            ea.grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
            ea.grfAccessMode = GRANT_ACCESS; 
            ea.grfInheritance = NO_INHERITANCE;
            ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
            ea.Trustee.ptstrName = (LPSTR)pSID;
            if (SetEntriesInAclA(1, &ea, pOldDACL, &pNewDACL) == ERROR_SUCCESS) {
                SetNamedSecurityInfoA(path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, pNewDACL, NULL);
                LocalFree(pNewDACL);
            }
            LocalFree(pSID);
        }
        LocalFree(pSD);
    }
}

static std::atomic<bool> g_applyingSettings{false};
static std::atomic<bool> g_pendingMeasureOverride{false};
static std::atomic<double> g_frameSizeOverrideOffset{0.0};
static std::atomic<bool> g_restoring{false};
static std::atomic<DWORD> g_taskbarPos{ABE_BOTTOM};

static UINT g_msgSetPosition = 0;
static UINT g_msgRestore = 0;
static HANDLE g_hSharedMem = nullptr; 

static int g_originalTaskbarHeight = 0;
static bool g_inSystemTrayController_UpdateFrameSize = false;

const wchar_t* SHARED_MEM_NAME = L"Local\\SlimBar11_Config_Mem";
struct SharedConfig { DWORD taskbarPos; };

static bool IsVerticalMode() { 
    DWORD pos = g_taskbarPos.load();
    return pos == ABE_LEFT || pos == ABE_RIGHT; 
}

static bool GetMonitorRect(HMONITOR monitor, RECT* rc) {
    MONITORINFO mi{ sizeof(MONITORINFO) };
    return GetMonitorInfoA(monitor, &mi) && CopyRect(rc, &mi.rcMonitor);
}

static UINT GetSafeDpi(HWND hWnd = nullptr) {
    if (!hWnd) hWnd = FindTray();
    UINT dpi = (hWnd && IsWindow(hWnd)) ? GetDpiForWindow(hWnd) : 96;
    return dpi ? dpi : 96;
}

static void GetDpiForMonitorSafe(HMONITOR mon, UINT* dpiX, UINT* dpiY) {
    if (mon && g_pGetDpiForMonitor) {
        if (SUCCEEDED(g_pGetDpiForMonitor(mon, 0, dpiX, dpiY)) && *dpiY > 0) return;
    }
    *dpiX = *dpiY = GetSafeDpi();
}

static int GetDynamicPhysicalHeight(HMONITOR mon = nullptr) {
    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitorSafe(mon, &dpiX, &dpiY);
    return MulDiv(TASKBAR_HEIGHT, dpiY, 96);
}

static int GetDynamicPhysicalWidth(HMONITOR mon = nullptr) {
    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitorSafe(mon, &dpiX, &dpiY);
    return MulDiv(TASKBAR_WIDTH_VERTICAL, dpiX, 96);
}

struct SymbolEntry { const char* name; void** ppOriginal; void* hookFunc; bool optional; bool found; const char* exclude; };
struct EnumContext { std::vector<SymbolEntry>* entries; HMODULE moduleBase; int foundCount; int totalEnumerated; };

static BOOL CALLBACK SymEnumCallback(PSYMBOL_INFO pSym, ULONG, PVOID ctx) {
    auto* c = (EnumContext*)ctx; 
    c->totalEnumerated++;
    const char* n = pSym->Name;
    if (strstr(n, "dtor$") || strstr(n, "catch$") || strstr(n, "__scrt") || strstr(n, "`dynamic") || strstr(n, "`scalar") || strstr(n, "`vector") || n[0] == '`') return TRUE;
    for (auto& e : *c->entries) {
        if (e.found) continue;
        if (strstr(n, e.name)) {
            if (e.exclude && strstr(n, e.exclude)) continue; 
            *e.ppOriginal = (void*)pSym->Address; 
            e.found = true; 
            c->foundCount++; 
            break;
        }
    }
    return TRUE;
}

struct CV_INFO_PDB70 { DWORD Signature; GUID PdbGuid; DWORD Age; char PdbFileName[1]; };
static bool GetPdbInfo(HMODULE mod, GUID& guid, DWORD& age, std::string& name) {
    auto dos = (PIMAGE_DOS_HEADER)mod; 
    if (IsBadReadPtr(dos, sizeof(IMAGE_DOS_HEADER)) || dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = (PIMAGE_NT_HEADERS64)((BYTE*)mod + dos->e_lfanew); 
    if (IsBadReadPtr(nt, sizeof(IMAGE_NT_HEADERS64)) || nt->Signature != IMAGE_NT_SIGNATURE) return false;
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (!dir.VirtualAddress || !dir.Size) return false;
    
    auto entries = (PIMAGE_DEBUG_DIRECTORY)((BYTE*)mod + dir.VirtualAddress);
    if (IsBadReadPtr(entries, dir.Size)) return false;
    for (int i = 0; i < (int)(dir.Size / sizeof(IMAGE_DEBUG_DIRECTORY)); i++) {
        if (entries[i].Type == IMAGE_DEBUG_TYPE_CODEVIEW) {
            auto cv = (CV_INFO_PDB70*)((BYTE*)mod + entries[i].AddressOfRawData);
            if (!IsBadReadPtr(cv, sizeof(CV_INFO_PDB70)) && cv->Signature == 0x53445352) { 
                guid = cv->PdbGuid; age = cv->Age; name = cv->PdbFileName; return true; 
            }
        }
    }
    return false;
}

static std::string FormatGuid(const GUID& g) {
    char b[64]; sprintf_s(b, "%08X%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X", g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]); return b;
}

static bool DownloadFile(const wchar_t* host, const wchar_t* path, const char* save, const char* displayName, int line) {
    HINTERNET hS = WinHttpOpen(L"TT/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, 0, 0, 0); 
    if (!hS) return false;
    WinHttpSetTimeouts(hS, 10000, 10000, 10000, 30000);
    HINTERNET hC = WinHttpConnect(hS, host, INTERNET_DEFAULT_HTTPS_PORT, 0); 
    if (!hC) { WinHttpCloseHandle(hS); return false; }
    HINTERNET hR = WinHttpOpenRequest(hC, L"GET", path, 0, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hR) { WinHttpCloseHandle(hC); WinHttpCloseHandle(hS); return false; }
    
    bool ok = false;
    std::string tmpSave = std::string(save) + ".tmp"; 

    if (WinHttpSendRequest(hR, 0, 0, 0, 0, 0, 0) && WinHttpReceiveResponse(hR, 0)) {
        DWORD st = 0, sz = sizeof(st); 
        WinHttpQueryHeaders(hR, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, 0, &st, &sz, 0);
        if (st == 200) {
            DWORD cl = 0, ds = sizeof(cl); 
            WinHttpQueryHeaders(hR, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &cl, &ds, WINHTTP_NO_HEADER_INDEX);
            
            FILE* f = fopen(tmpSave.c_str(), "wb");
            if (f) {
                BYTE buf[8192]; DWORD rd = 0, tot = 0;
                while (WinHttpReadData(hR, buf, sizeof(buf), &rd)) {
                    if (!rd) break; fwrite(buf, 1, rd, f); tot += rd; 
                    if (cl > 0) PrintAt(line, "[%-16s] Downloading: %7u / %7u bytes (%5.1f%%)", displayName, tot, cl, (float)tot * 100.0f / cl);
                    else PrintAt(line, "[%-16s] Downloading: %7u bytes ...", displayName, tot);
                }
                PrintAt(line, "[%-16s] Download complete.", displayName);
                fclose(f); 
                if (tot > 0 && (cl == 0 || tot == cl)) {
                    MoveFileExA(tmpSave.c_str(), save, MOVEFILE_REPLACE_EXISTING);
                    ok = true;
                } else {
                    DeleteFileA(tmpSave.c_str());
                }
            }
        }
    }
    WinHttpCloseHandle(hR); WinHttpCloseHandle(hC); WinHttpCloseHandle(hS); 
    return ok;
}

static std::string GetSymbolDir() {
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, path))) {
        std::string dir = std::string(path) + "\\SlimBar11";
        CreateDirectoryA(dir.c_str(), 0);
        dir += "\\symbols";
        CreateDirectoryA(dir.c_str(), 0);
        return dir;
    }
    return GetModuleDir() + "\\symbols"; 
}

static bool EnsurePdb(HMODULE mod, const std::string& cache, std::string& out, const char* displayName, int line) {
    GUID g; DWORD a; std::string n; 
    if (!GetPdbInfo(mod, g, a, n)) return false;
    std::string sig = FormatGuid(g); char ab[16]; sprintf_s(ab, "%X", a); sig += ab;
    std::string dir = cache + "\\" + n + "\\" + sig; out = dir + "\\" + n;
    if (GetFileAttributesA(out.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    CreateDirectoryA((cache + "\\" + n).c_str(), 0); CreateDirectoryA(dir.c_str(), 0);
    std::string url = "/download/symbols/" + n + "/" + sig + "/" + n;
    wchar_t wp[1024]; MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, wp, 1024);
    return DownloadFile(L"msdl.microsoft.com", wp, out.c_str(), displayName, line);
}

static std::string GetModSig(HMODULE m) { GUID g; DWORD a; std::string n; if (!GetPdbInfo(m, g, a, n)) return ""; char ab[16]; sprintf_s(ab, "%X", a); return n + "_" + FormatGuid(g) + ab; }

static bool LoadCache(HMODULE mod, const std::string& sig, std::vector<SymbolEntry>& ents) {
    std::string ini = GetModuleDir() + "\\slimbar11_symbols.ini";
    for (auto& e : ents) { char v[32]; GetPrivateProfileStringA(sig.c_str(), e.name, "", v, 32, ini.c_str()); if (!v[0] && !e.optional) return false; }
    int cnt = 0;
    for (auto& e : ents) { char v[32]; GetPrivateProfileStringA(sig.c_str(), e.name, "", v, 32, ini.c_str()); if (v[0]) { *e.ppOriginal = (void*)((BYTE*)mod + strtoul(v, 0, 16)); e.found = true; cnt++; } }
    return cnt > 0;
}

static void SaveCache(HMODULE mod, const std::string& sig, const std::vector<SymbolEntry>& ents) {
    std::string ini = GetModuleDir() + "\\slimbar11_symbols.ini";
    for (auto& e : ents) { if (e.found && *e.ppOriginal) { char v[32]; sprintf_s(v, "%X", (DWORD)((BYTE*)*e.ppOriginal - (BYTE*)mod)); WritePrivateProfileStringA(sig.c_str(), e.name, v, ini.c_str()); } }
    WritePrivateProfileStringA(NULL, NULL, NULL, ini.c_str()); 
}

static bool g_symInit = false; static std::string g_symDir;
static std::atomic<int> g_symRefCount{0};
static std::mutex g_symMutex;
static std::set<std::string> g_currentlyResolving;
static std::condition_variable g_resolvingCv;
static std::mutex g_resolvingMutex;

static void CloseConsole() {
    printf("\nAll symbols resolved. Closing in 2 seconds...\n");
    Sleep(2000); 
    HWND hwnd = GetConsoleWindow();
    if (hwnd) PostMessage(hwnd, WM_CLOSE, 0, 0);
    FreeConsole();
    FILE* f;
    freopen_s(&f, "NUL", "w", stdout);
    freopen_s(&f, "NUL", "w", stderr);
}

static bool StartSymSession() {
    std::lock_guard<std::mutex> lock(g_symMutex);
    if (g_symRefCount++ == 0) {
        g_symDir = GetSymbolDir();
        OpenConsole();
        SymSetOptions(SYMOPT_DEFERRED_LOADS);
        SymCleanup(GetCurrentProcess());
        if (!SymInitialize(GetCurrentProcess(), g_symDir.c_str(), FALSE)) {
            printf("Failed to initialize symbols: %lu\n", GetLastError());
            g_symRefCount--;
            return false;
        }
        SymSetSearchPath(GetCurrentProcess(), g_symDir.c_str());
        g_symInit = true;
    }
    return true;
}

static void EndSymSession() {
    std::lock_guard<std::mutex> lock(g_symMutex);
    if (--g_symRefCount <= 0) {
        g_symRefCount = 0;
        if (g_symInit) {
            SymCleanup(GetCurrentProcess());
            g_symInit = false;
            CloseConsole();
        }
    }
}

static bool InitSym() { 
    return StartSymSession();
}

static bool Resolve(HMODULE mod, std::vector<SymbolEntry>& ents) {
    bool anyMissing = false;
    for (auto& e : ents) { if (e.ppOriginal && *e.ppOriginal) e.found = true; else anyMissing = true; }
    if (!anyMissing) return true;

    std::string sig = GetModSig(mod); if (sig.empty()) return false;
    std::string sdn = sig.substr(0, sig.find('_'));
    const char* displayName = (mod == GetModuleHandleW(NULL)) ? "Main" : sdn.c_str();

    {
        std::unique_lock<std::mutex> lock(g_resolvingMutex);
        while (g_currentlyResolving.count(sig)) {
            g_resolvingCv.wait(lock);
            anyMissing = false;
            for (auto& e : ents) { if (e.ppOriginal && *e.ppOriginal) e.found = true; else anyMissing = true; }
            if (!anyMissing) return true;
            if (LoadCache(mod, sig, ents)) return true;
        }
        g_currentlyResolving.insert(sig);
    }

    bool success = false;
    if (LoadCache(mod, sig, ents)) { success = true; }
    else if (StartSymSession()) {
        int line = g_nextProgressLine.fetch_add(1);
        std::string pdb; 
        if (EnsurePdb(mod, g_symDir, pdb, displayName, line)) {
            BYTE* basePtr = (BYTE*)((ULONG_PTR)mod & ~3);
            DWORD sizeOfImage = 0;
            MODULEINFO mi{}; 
            if (GetModuleInformation(GetCurrentProcess(), mod, &mi, sizeof(mi))) sizeOfImage = mi.SizeOfImage;
            else {
                auto dos = (PIMAGE_DOS_HEADER)basePtr;
                if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
                    auto nt = (PIMAGE_NT_HEADERS64)(basePtr + dos->e_lfanew);
                    if (nt->Signature == IMAGE_NT_SIGNATURE) sizeOfImage = nt->OptionalHeader.SizeOfImage;
                }
            }
            char path[MAX_PATH]{}; GetModuleFileNameA(mod, path, MAX_PATH);
            DWORD64 base = SymLoadModuleEx(GetCurrentProcess(), 0, path, 0, (DWORD64)basePtr, sizeOfImage, 0, 0);
            if (base || GetLastError() == ERROR_SUCCESS) {
                EnumContext ctx{ &ents, mod, 0, 0 };
                PrintAt(line, "[%-16s] Scanning symbols...", displayName);
                SymEnumSymbols(GetCurrentProcess(), (DWORD64)basePtr, "*", (PSYM_ENUMERATESYMBOLS_CALLBACK)SymEnumCallback, &ctx);
                SymUnloadModule64(GetCurrentProcess(), (DWORD64)basePtr);
                if (ctx.foundCount > 0) {
                    PrintAt(line, "[%-16s] Saving %d symbols to cache...", displayName, ctx.foundCount);
                    SaveCache(mod, sig, ents);
                } else {
                    PrintAt(line, "[%-16s] No symbols found.", displayName);
                }
                success = true;
            }
            DeleteFileA(pdb.c_str());
        }
        EndSymSession();
    }
    
    {
        std::lock_guard<std::mutex> lock(g_resolvingMutex);
        g_currentlyResolving.erase(sig);
        g_resolvingCv.notify_all();
    }
    return success;
}

// ===== Original API Pointers =====
static void (WINAPI* Orig_IconUtils_GetIconSize)(bool, int, SIZE*) = nullptr;
static bool (WINAPI* Orig_IconContainer_IsStorageRecreationRequired)(void*, void*, int) = nullptr;
static void (WINAPI* Orig_TrayUI_GetMinSize)(void*, HMONITOR, SIZE*) = nullptr;
static ULONG_PTR (WINAPI* Orig_CIconLoadingFunctions_GetClassLongPtrW)(void*, HWND, int) = nullptr;
static BOOL (WINAPI* Orig_CIconLoadingFunctions_SendMessageCallbackW)(void*, HWND, UINT, WPARAM, LPARAM, SENDASYNCPROC, ULONG_PTR) = nullptr;
static void (WINAPI* Orig_TrayUI_StuckTrayChange)(void*) = nullptr;
static void (WINAPI* Orig_TrayUI_HandleSettingChange)(void*, void*, void*, void*, void*) = nullptr;
static DWORD (WINAPI* Orig_TrayUI_GetDockedRect)(void*, RECT*, BOOL) = nullptr;
static void  (WINAPI* Orig_TrayUI_MakeStuckRect)(void*, RECT*, RECT*, SIZE, DWORD) = nullptr;
static void  (WINAPI* Orig_TrayUI_GetStuckInfo)(void*, RECT*, DWORD*) = nullptr;
struct WFPoint { float X; float Y; };
static HRESULT (WINAPI* Orig_ComputeJumpViewPos)(void*, void*, int, WFPoint*, int*, int*) = nullptr;

static double* g_double48 = nullptr;
static double g_saved48 = -1.0;

static void (WINAPI* Orig_SystemTrayController_UpdateFrameSize)(void*) = nullptr;
static void (WINAPI* Orig_SystemTraySecondaryController_UpdateFrameSize)(void*) = nullptr;
static double (WINAPI* Orig_GetFrameSize)(int) = nullptr;
static double (WINAPI* Orig_GetIconH_Enum)(int) = nullptr;
static double (WINAPI* Orig_GetIconH_Dbl)(double) = nullptr;
static double (WINAPI* Orig_GetIconH_Method)(void*) = nullptr;
static double (WINAPI* Orig_SysTrayGetFrame)(void*, int) = nullptr;
static double (WINAPI* Orig_SysTray2GetFrame)(void*, int) = nullptr;

static decltype(&SHAppBarMessage) Orig_SHABM = nullptr;
static decltype(&LoadLibraryExW) Orig_LLEW = nullptr;
static decltype(&SetWindowPos) Orig_SWP = nullptr;
static decltype(&MoveWindow) Orig_MW = nullptr;
static decltype(&TrackPopupMenuEx) Orig_TrackPopupMenuEx = nullptr;
static decltype(&SystemParametersInfoW) Orig_SystemParametersInfoW = nullptr;
static decltype(&GetMonitorInfoW) Orig_GetMonitorInfoW = nullptr;

// GDI Thumbnail Hooks
static bool g_inCTaskListThumbnailWnd_DisplayUI = false;
static bool g_inCTaskListThumbnailWnd_LayoutThumbnails = false;
static void* (WINAPI* Orig_CTaskListThumbnailWnd_DisplayUI)(void*, void*, void*, void*, void*) = nullptr;
static void (WINAPI* Orig_CTaskListThumbnailWnd_LayoutThumbnails)(void*) = nullptr;

static void* WINAPI Hook_CTaskListThumbnailWnd_DisplayUI(void* pThis, void* p1, void* p2, void* p3, void* p4) {
    g_inCTaskListThumbnailWnd_DisplayUI = true;
    void* ret = nullptr;
    if (Orig_CTaskListThumbnailWnd_DisplayUI) ret = Orig_CTaskListThumbnailWnd_DisplayUI(pThis, p1, p2, p3, p4);
    g_inCTaskListThumbnailWnd_DisplayUI = false;
    return ret;
}

static void WINAPI Hook_CTaskListThumbnailWnd_LayoutThumbnails(void* pThis) {
    g_inCTaskListThumbnailWnd_LayoutThumbnails = true;
    if (Orig_CTaskListThumbnailWnd_LayoutThumbnails) Orig_CTaskListThumbnailWnd_LayoutThumbnails(pThis);
    g_inCTaskListThumbnailWnd_LayoutThumbnails = false;
}

static void RefreshTaskSw(HWND hTray) {
    HWND hReBar = FindWindowEx(hTray, nullptr, L"ReBarWindow32", nullptr);
    if (hReBar) { 
        HWND hTaskSw = FindWindowEx(hReBar, nullptr, L"MSTaskSwWClass", nullptr); 
        if (hTaskSw) SendMessage(hTaskSw, 0x452, 3, 0); 
    }
}

// ===== Hooks =====
static void WINAPI Hook_GetIconSize(bool isSmall, int type, SIZE* sz) {
    Orig_IconUtils_GetIconSize(isSmall, type, sz);
    if (!isSmall && !g_restoring) { 
        sz->cx = MulDiv(sz->cx, ICON_SIZE, 24); 
        sz->cy = MulDiv(sz->cy, ICON_SIZE, 24); 
    }
}

static bool WINAPI Hook_IsStorageRecreation(void* p, void* p1, int f) {
    if (g_applyingSettings) return true;
    return Orig_IconContainer_IsStorageRecreationRequired(p, p1, f);
}

static ULONG_PTR WINAPI Hook_GetClassLongPtrW(void* p, HWND hw, int idx) {
    if (idx == GCLP_HICON && ICON_SIZE <= 16 && !g_restoring) idx = GCLP_HICONSM;
    return Orig_CIconLoadingFunctions_GetClassLongPtrW(p, hw, idx);
}

static BOOL WINAPI Hook_SendMsgCbW(void* p, HWND hw, UINT msg, WPARAM wp, LPARAM lp, SENDASYNCPROC cb, ULONG_PTR data) {
    if (msg == WM_GETICON && wp == ICON_BIG && ICON_SIZE <= 16 && !g_restoring) wp = ICON_SMALL2;
    return Orig_CIconLoadingFunctions_SendMessageCallbackW(p, hw, msg, wp, lp, cb, data);
}

static void WINAPI Hook_GetMinSize(void* p, HMONITOR mon, SIZE* sz) {
    Orig_TrayUI_GetMinSize(p, mon, sz);
    if (g_restoring) {
        g_pendingMeasureOverride = false;
        return;
    }
    if (IsVerticalMode()) sz->cx = GetDynamicPhysicalWidth(mon);
    else sz->cy = GetDynamicPhysicalHeight(mon);
    g_pendingMeasureOverride = false;
}

static void WINAPI Hook_HandleSettingChange(void* p, void* a, void* b, void* c, void* d) {
    Orig_TrayUI_HandleSettingChange(p, a, b, c, d);
    if (g_applyingSettings && Orig_TrayUI_StuckTrayChange) Orig_TrayUI_StuckTrayChange(p);
}

static DWORD WINAPI Hook_GetDockedRect(void* p, RECT* rect, BOOL p2) {
    DWORD ret = Orig_TrayUI_GetDockedRect(p, rect, p2);
    if (g_restoring) return ret;
    
    DWORD pos = g_taskbarPos.load();
    HMONITOR mon = MonitorFromRect(rect, MONITOR_DEFAULTTONEAREST);
    RECT rc; if (!GetMonitorRect(mon, &rc)) return ret;
    
    int h = GetDynamicPhysicalHeight(mon);
    int w = GetDynamicPhysicalWidth(mon);

    switch (pos) {
        case ABE_BOTTOM: rect->top = rc.bottom - h; rect->bottom = rc.bottom; rect->left = rc.left; rect->right = rc.right; break;
        case ABE_TOP:    rect->top = rc.top; rect->bottom = rc.top + h; rect->left = rc.left; rect->right = rc.right; break; 
        case ABE_LEFT:   rect->top = rc.top; rect->bottom = rc.bottom; rect->left = rc.left; rect->right = rc.left + w; break; 
        case ABE_RIGHT:  rect->top = rc.top; rect->bottom = rc.bottom; rect->right = rc.right; rect->left = rc.right - w; break; 
    }
    return ret;
}

static void WINAPI Hook_MakeStuckRect(void* p, RECT* rect, RECT* p2, SIZE p3, DWORD p4) {
    Orig_TrayUI_MakeStuckRect(p, rect, p2, p3, p4);
    if (g_restoring) return;
    
    DWORD targetPos = g_taskbarPos.load();
    HMONITOR mon = MonitorFromRect(rect, MONITOR_DEFAULTTONEAREST); 
    RECT rc; if (!GetMonitorRect(mon, &rc)) return;
    
    int h = GetDynamicPhysicalHeight(mon);
    int w = GetDynamicPhysicalWidth(mon);

    switch (targetPos) {
        case ABE_BOTTOM: rect->top = rc.bottom - h; rect->bottom = rc.bottom; rect->left = rc.left; rect->right = rc.right; break;
        case ABE_TOP:    rect->top = rc.top; rect->bottom = rc.top + h; rect->left = rc.left; rect->right = rc.right; break; 
        case ABE_LEFT:   rect->left = rc.left; rect->right = rc.left + w; rect->top = rc.top; rect->bottom = rc.bottom; break; 
        case ABE_RIGHT:  rect->left = rc.right - w; rect->right = rc.right; rect->top = rc.top; rect->bottom = rc.bottom; break; 
    }
}

static void WINAPI Hook_GetStuckInfo(void* p, RECT* rect, DWORD* pos) {
    Orig_TrayUI_GetStuckInfo(p, rect, pos);
    if (g_restoring) return;
    
    *pos = g_taskbarPos.load();
    if (*pos != ABE_BOTTOM && rect) {
        HMONITOR mon = MonitorFromRect(rect, MONITOR_DEFAULTTONEAREST); 
        RECT rc;
        if (GetMonitorRect(mon, &rc)) {
            int h = rect->bottom - rect->top; 
            switch (*pos) {
                case ABE_TOP: rect->left = rc.left; rect->right = rc.right; rect->top = rc.top; rect->bottom = rc.top + h; break;
                case ABE_LEFT:  { int w = GetDynamicPhysicalWidth(mon); rect->left = rc.left; rect->right = rc.left + w; rect->top = rc.top; rect->bottom = rc.bottom; break; }
                case ABE_RIGHT: { int w = GetDynamicPhysicalWidth(mon); rect->left = rc.right - w; rect->right = rc.right; rect->top = rc.top; rect->bottom = rc.bottom; break; }
            }
        }
    }
}

static double WINAPI Hook_GetFrameSize(int e) {
    if (!g_originalTaskbarHeight && (e == 1 || e == 2)) g_originalTaskbarHeight = (int)Orig_GetFrameSize(e);
    if (g_restoring) return Orig_GetFrameSize(e);
    if (!IsVerticalMode() && (e == 1 || e == 2)) return (double)TASKBAR_HEIGHT + g_frameSizeOverrideOffset.load();
    return Orig_GetFrameSize(e);
}

static double WINAPI Hook_GetIconH_Enum(int e) { return g_restoring ? Orig_GetIconH_Enum(e) : (double)ICON_SIZE; }
static double WINAPI Hook_GetIconH_Dbl(double b) { return g_restoring ? Orig_GetIconH_Dbl(b) : (double)ICON_SIZE; }
static double WINAPI Hook_GetIconH_Method(void* p) { return g_restoring ? Orig_GetIconH_Method(p) : (double)ICON_SIZE; }

static LONG GetLastHeightOffset() {
    static LONG off = []() -> LONG {
        if (!Orig_SystemTrayController_UpdateFrameSize) return 0;
#if defined(_M_X64)
        if (IsBadReadPtr(Orig_SystemTrayController_UpdateFrameSize, 0x200)) return 0;
        const BYTE* s = (const BYTE*)Orig_SystemTrayController_UpdateFrameSize;
        for (const BYTE* p = s; p != s + 0x200; p++) {
            if (p[0] == 0x66 && p[1] == 0x0F && p[2] == 0x2E && p[3] == 0xB3 && p[8] == 0x7A && p[10] == 0x75) { 
                LONG o = *(LONG*)(p + 4); 
                return (o < 0 || o > 0xFFFF) ? 0 : o; 
            }
        }
#endif
        return 0;
    }(); 
    return off;
}

static void WINAPI Hook_SystemTrayController_UpdateFrameSize(void* pThis) {
    if (IsVerticalMode()) { 
        if (Orig_SystemTrayController_UpdateFrameSize) Orig_SystemTrayController_UpdateFrameSize(pThis); 
        return; 
    }
    LONG o = GetLastHeightOffset(); 
    if (o && !IsBadWritePtr((BYTE*)pThis + o, sizeof(double))) *(double*)((BYTE*)pThis + o) = 0.0;
    
    g_inSystemTrayController_UpdateFrameSize = true;
    if (Orig_SystemTrayController_UpdateFrameSize) Orig_SystemTrayController_UpdateFrameSize(pThis);
    g_inSystemTrayController_UpdateFrameSize = false;
}

static void WINAPI Hook_SystemTraySecondaryController_UpdateFrameSize(void* pThis) {
    if (IsVerticalMode()) { 
        if (Orig_SystemTraySecondaryController_UpdateFrameSize) Orig_SystemTraySecondaryController_UpdateFrameSize(pThis); 
        return; 
    }
    LONG o = GetLastHeightOffset(); 
    if (o && !IsBadWritePtr((BYTE*)pThis + o, sizeof(double))) *(double*)((BYTE*)pThis + o) = 0.0;
    
    g_inSystemTrayController_UpdateFrameSize = true;
    if (Orig_SystemTraySecondaryController_UpdateFrameSize) Orig_SystemTraySecondaryController_UpdateFrameSize(pThis);
    g_inSystemTrayController_UpdateFrameSize = false;
}

static double WINAPI Hook_SysTrayFrame(void* p, int e) {
    if (g_restoring) return Orig_SysTrayGetFrame(p, e);
    if (!IsVerticalMode() && (e == 1 || e == 2)) return (double)TASKBAR_HEIGHT + g_frameSizeOverrideOffset.load();
    return Orig_SysTrayGetFrame(p, e);
}

static double WINAPI Hook_SysTray2Frame(void* p, int e) {
    if (g_restoring) return Orig_SysTray2GetFrame(p, e);
    if (!IsVerticalMode() && (e == 1 || e == 2)) return (double)TASKBAR_HEIGHT + g_frameSizeOverrideOffset.load();
    return Orig_SysTray2GetFrame(p, e);
}

static HWINEVENTHOOK g_jumpListEventHook = nullptr;
static DWORD g_jumpViewTid = 0; 

static void CALLBACK JumpListWinEventProc(HWINEVENTHOOK, DWORD, HWND hWnd, LONG idObject, LONG, DWORD, DWORD) {
    if (!hWnd || idObject != OBJID_WINDOW) return; 

    DWORD tid = GetWindowThreadProcessId(hWnd, nullptr);
    if (!tid) return;

    if (tid != g_jumpViewTid) {
        WCHAR cls[64];
        if (!GetClassNameW(hWnd, cls, 64) || _wcsicmp(cls, L"Windows.UI.Core.CoreWindow") != 0) return;
        
        HANDLE hThread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
        if (!hThread) return;
        
        bool isJumpView = false;
        typedef HRESULT(WINAPI* GTD)(HANDLE, PWSTR*);
        static GTD pGTD = (GTD)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
        if (pGTD) { 
            PWSTR desc = nullptr; 
            if (SUCCEEDED(pGTD(hThread, &desc)) && desc) { 
                if (wcscmp(desc, L"JumpViewUI") == 0) {
                    isJumpView = true;
                    g_jumpViewTid = tid; 
                }
                LocalFree(desc); 
            } 
        }
        CloseHandle(hThread);
        if (!isJumpView) return;
    }

    HWND hTray = FindWindowW(L"Shell_TrayWnd", nullptr); 
    if (!hTray) return;
    RECT trayRc; GetWindowRect(hTray, &trayRc); 
    if (trayRc.top != 0 || (trayRc.bottom - trayRc.top) >= 100) return;

    RECT rc; GetWindowRect(hWnd, &rc); 
    int targetY = trayRc.bottom; 
    
    if (rc.top != targetY) {
        SetWindowPos(hWnd, nullptr, rc.left, targetY, 0, 0, SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    }
}

static void InstallJumpListEventHook() { 
    if (!g_jumpListEventHook) {
        g_jumpListEventHook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, JumpListWinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS); 
    }
}

static void UninstallJumpListEventHook() { 
    if (g_jumpListEventHook) { 
        UnhookWinEvent(g_jumpListEventHook); 
        g_jumpListEventHook = nullptr; 
    } 
}

static void ApplySettings(HWND hMsgHost = nullptr);
static BOOL WINAPI Hook_SystemParametersInfoW(UINT uiAction, UINT uiParam, PVOID pvParam, UINT fWinIni);
static BOOL WINAPI Hook_GetMonitorInfoW(HMONITOR hMonitor, LPMONITORINFO lpmi);

static HRESULT WINAPI Hook_ComputeJumpViewPos(void* pThis, void* tbg, int p2, WFPoint* pt, int* hA, int* vA) {
    HRESULT ret = Orig_ComputeJumpViewPos(pThis, tbg, p2, pt, hA, vA);
    if (!g_restoring && g_taskbarPos.load() == ABE_TOP && pt) {
        POINT c; GetCursorPos(&c); 
        HMONITOR mon = MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST); 
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfoW(mon, &mi)) { 
            pt->Y = (float)(mi.rcWork.bottom - 1); 
        }
    }
    return ret;
}

static LRESULT CALLBACK PosFixSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    if (uMsg == WM_APP + 103) { RemoveWindowSubclass(hWnd, PosFixSubclassProc, 1); return 0; }
    if (g_restoring) return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    
    DWORD pos = g_taskbarPos.load();
    if (uMsg == 0x5C3 && pos != ABE_BOTTOM) { if (wParam == ABE_BOTTOM) wParam = pos; }

    if (uMsg == WM_DPICHANGED || uMsg == WM_DISPLAYCHANGE) {
        HWND hMsg = FindWindowW(L"SlimBar11_MsgHost", nullptr);
        if (hMsg) PostMessageW(hMsg, WM_APP + 104, 0, 0); 
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

static auto WINAPI Hook_SHABM(DWORD msg, PAPPBARDATA d) -> decltype(SHAppBarMessage(msg, d)) {
    static bool g_subclassed = false;
    if (!g_subclassed && d && d->hWnd) { SetWindowSubclass(d->hWnd, PosFixSubclassProc, 1, 0); g_subclassed = true; }
    
    auto r = Orig_SHABM(msg, d);
    
    if (!g_restoring && !IsVerticalMode()) {
        DWORD curPos = g_taskbarPos.load();
        HMONITOR mon = MonitorFromWindow(d->hWnd, MONITOR_DEFAULTTONEAREST); 
        int th = GetDynamicPhysicalHeight(mon);
        
        if (msg == ABM_QUERYPOS && r) {
            if (curPos == ABE_TOP) {
                RECT rc; if (GetMonitorRect(mon, &rc)) { d->rc.left = rc.left; d->rc.right = rc.right; d->rc.top = rc.top; d->rc.bottom = rc.top + th; }
                d->uEdge = ABE_TOP;
            } else if (curPos == ABE_BOTTOM) { d->rc.top = d->rc.bottom - th; }
        }
        if (msg == ABM_GETTASKBARPOS && r) {
            if (curPos == ABE_TOP) {
                RECT rc; if (GetMonitorRect(mon, &rc)) { d->rc.left = rc.left; d->rc.right = rc.right; d->rc.top = rc.top; d->rc.bottom = rc.top + th; d->uEdge = ABE_TOP; }
            }
        }
    }
    if (msg == ABM_QUERYPOS || msg == ABM_GETTASKBARPOS) g_pendingMeasureOverride = false;
    return r;
}

static std::atomic<bool> g_viewLoaded{false};
static HMODULE GetViewMod(bool forExecution = true) { 
    HMODULE m = GetModuleHandleW(L"Taskbar.View.dll"); 
    if (!m) m = GetModuleHandleW(L"ExplorerExtensions.dll"); 
    if (forExecution) return m;

    if (!m) m = LoadLibraryExW(L"Taskbar.View.dll", NULL, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) m = LoadLibraryExW(L"ExplorerExtensions.dll", NULL, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_SEARCH_SYSTEM32);
    return m; 
}

static bool SetupViewHooks(HMODULE mod);

static HMODULE WINAPI Hook_LLEW(LPCWSTR fn, HANDLE hf, DWORD fl) {
    HMODULE m = Orig_LLEW(fn, hf, fl);
    if (m && !g_viewLoaded && GetViewMod() == m) { 
        if (!g_viewLoaded.exchange(true)) {
            SetupViewHooks(m);
            ApplySettings();
        }
    }
    return m;
}

struct DE { void** pp; void* hk; };
static std::vector<DE> g_hooks;

static bool ApplyD(const std::vector<DE>& es) {
    DetourTransactionBegin(); 
    DetourUpdateThread(GetCurrentThread());
    for (auto& e : es) { if (*e.pp && e.hk) { DetourAttach(e.pp, e.hk); g_hooks.push_back(e); } }
    return DetourTransactionCommit() == NO_ERROR;
}

static void CleanupD() {
    if (g_hooks.empty()) return;
    DetourTransactionBegin(); 
    DetourUpdateThread(GetCurrentThread());
    for (auto& e : g_hooks) DetourDetach(e.pp, e.hk);
    DetourTransactionCommit(); 
    g_hooks.clear();
}

using MenuFlyout_ShowAt_t = void*(WINAPI*)(void*, DependencyObject*, Controls::Primitives::FlyoutShowOptions*);
using ShowContextMenu_t = void(WINAPI*)(void*);
static MenuFlyout_ShowAt_t Orig_MenuFlyout_ShowAt = nullptr;
static ShowContextMenu_t Orig_TextIconContent_ShowContextMenu = nullptr;
static ShowContextMenu_t Orig_DateTimeIconContent_ShowContextMenu = nullptr;

static FrameworkElement FindChildByName(DependencyObject const& parent, std::wstring const& name) {
    if (!parent) return nullptr;
    int count = Media::VisualTreeHelper::GetChildrenCount(parent);
    for (int i = 0; i < count; i++) {
        DependencyObject child = Media::VisualTreeHelper::GetChild(parent, i);
        if (auto element = child.try_as<FrameworkElement>()) { if (element.Name() == name) return element; }
        if (auto result = FindChildByName(child, name)) return result;
    } 
    return nullptr;
}

static bool HandleSystemTrayContextMenu(FrameworkElement const& element) {
    FrameworkElement childElement = FindChildByName(element, L"ContainerGrid"); 
    if (!childElement) return false;
    auto flyout = Controls::Primitives::FlyoutBase::GetAttachedFlyout(childElement); 
    if (!flyout) return false;
    
    Controls::Primitives::FlyoutShowOptions options;
    options.Position(winrt::Windows::Foundation::Point{static_cast<float>(childElement.ActualWidth()), static_cast<float>(childElement.ActualHeight())});
    flyout.ShowAt(childElement, options); 
    return true;
}

static void* WINAPI Hook_MenuFlyout_ShowAt(void* pThis, DependencyObject* placementTarget, Controls::Primitives::FlyoutShowOptions* showOptions) {
    if (!showOptions || g_restoring || g_taskbarPos.load() != ABE_TOP) return Orig_MenuFlyout_ShowAt(pThis, placementTarget, showOptions);
    
    auto placement = showOptions->Placement();
    if (placement == Controls::Primitives::FlyoutPlacementMode::Top) {
        showOptions->Placement(Controls::Primitives::FlyoutPlacementMode::Bottom);
    }

    auto point = showOptions->Position().try_as<winrt::Windows::Foundation::Point>();
    if (point) {
        if (point->Y < 0) {
            point->Y = -point->Y;
            FrameworkElement targetElement = placementTarget->try_as<FrameworkElement>();
            if (targetElement) {
                point->Y += targetElement.ActualHeight();
            }
            showOptions->Position(point);
        }
    }
    
    return Orig_MenuFlyout_ShowAt(pThis, placementTarget, showOptions);
}

static IUnknown* SafeGetUnk1(void* pThis) {
    if (!pThis || IsBadReadPtr(pThis, sizeof(void*)*2)) return nullptr;
    __try { return ((IUnknown**)pThis)[1]; } 
    __except(EXCEPTION_EXECUTE_HANDLER) { return nullptr; } 
}

static void WINAPI Hook_TextIconContent_ShowContextMenu(void* pThis) {
    bool handled = false;
    if (!g_restoring && g_taskbarPos.load() == ABE_TOP) {
        IUnknown* pUnk = SafeGetUnk1(pThis);
        if (pUnk) { 
            FrameworkElement element = nullptr; 
            pUnk->QueryInterface(winrt::guid_of<FrameworkElement>(), winrt::put_abi(element)); 
            if (element && HandleSystemTrayContextMenu(element)) handled = true; 
        }
    }
    if (!handled && Orig_TextIconContent_ShowContextMenu) Orig_TextIconContent_ShowContextMenu(pThis);
}

static void WINAPI Hook_DateTimeIconContent_ShowContextMenu(void* pThis) {
    bool handled = false;
    if (!g_restoring && g_taskbarPos.load() == ABE_TOP) {
        IUnknown* pUnk = SafeGetUnk1(pThis);
        if (pUnk) { 
            FrameworkElement element = nullptr; 
            pUnk->QueryInterface(winrt::guid_of<FrameworkElement>(), winrt::put_abi(element)); 
            if (element && HandleSystemTrayContextMenu(element)) handled = true; 
        }
    }
    if (!handled && Orig_DateTimeIconContent_ShowContextMenu) Orig_DateTimeIconContent_ShowContextMenu(pThis);
}

static decltype(&MapWindowPoints) Orig_MapWindowPoints = nullptr;
static void (WINAPI* Orig_FlyoutFrame_UpdateFlyoutPosition)(void*) = nullptr;
static DWORD g_UpdateFlyoutPosition_threadId = 0;
static void* g_UpdateFlyoutPosition_pThis = nullptr;
static double g_lastFlyoutHeight = 0;

static void WINAPI Hook_FlyoutFrame_UpdateFlyoutPosition(void* pThis) {
    g_UpdateFlyoutPosition_threadId = GetCurrentThreadId();
    g_UpdateFlyoutPosition_pThis = pThis;
    if (Orig_FlyoutFrame_UpdateFlyoutPosition) Orig_FlyoutFrame_UpdateFlyoutPosition(pThis);
    g_UpdateFlyoutPosition_threadId = 0;
    g_UpdateFlyoutPosition_pThis = nullptr;
}

static int WINAPI Hook_MapWindowPoints(HWND hWndFrom, HWND hWndTo, LPPOINT lpPoints, UINT cPoints) {
    int ret = Orig_MapWindowPoints(hWndFrom, hWndTo, lpPoints, cPoints);

    if (GetCurrentThreadId() != g_UpdateFlyoutPosition_threadId || !g_UpdateFlyoutPosition_pThis || cPoints != 1) {
        return ret;
    }

    if (g_taskbarPos.load() != ABE_TOP) return ret;

    FrameworkElement flyoutFrame = nullptr;
    try {
        IUnknown* pUnk = (IUnknown*)g_UpdateFlyoutPosition_pThis;
        if (pUnk && !IsBadReadPtr(pUnk, sizeof(void*))) {
            pUnk->QueryInterface(winrt::guid_of<FrameworkElement>(), winrt::put_abi(flyoutFrame));
        }
        if (!flyoutFrame && !IsBadReadPtr(g_UpdateFlyoutPosition_pThis, sizeof(void*) * 2)) {
            IUnknown* pUnk2 = ((IUnknown**)g_UpdateFlyoutPosition_pThis)[1];
            if (pUnk2 && !IsBadReadPtr(pUnk2, sizeof(void*))) {
                pUnk2->QueryInterface(winrt::guid_of<FrameworkElement>(), winrt::put_abi(flyoutFrame));
            }
        }
    } catch (...) {}

    if (!flyoutFrame) return ret;

    FrameworkElement hoverFlyoutCanvas = FindChildByName(flyoutFrame, L"HoverFlyoutCanvas");
    if (!hoverFlyoutCanvas) return ret;

    FrameworkElement hoverFlyoutGrid = FindChildByName(hoverFlyoutCanvas, L"HoverFlyoutGrid");
    if (!hoverFlyoutGrid) return ret;

    auto flyoutSize = hoverFlyoutGrid.DesiredSize();
    g_lastFlyoutHeight = flyoutSize.Height;

    HWND hTray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (hTray) {
        RECT trayRc; GetWindowRect(hTray, &trayRc);
        UINT dpiY = GetSafeDpi(hTray);
        int flyoutHeight = MulDiv((int)flyoutSize.Height, dpiY, 96);
        
        int offsetToAdd = flyoutHeight;
        offsetToAdd += (trayRc.bottom - trayRc.top); 
        offsetToAdd += MulDiv(12, dpiY, 96); 

        lpPoints->y += offsetToAdd;
    }

    return ret;
}

static void (WINAPI* Orig_SystemTrayFrame_Height)(void*, double) = nullptr;
static void (WINAPI* Orig_TaskbarFrame_MaxHeight)(void*, double) = nullptr;
static void (WINAPI* Orig_TaskbarFrame_Height)(void*, double) = nullptr;
static void (WINAPI* Orig_TaskbarController_UpdateFrameHeight)(void*) = nullptr;
static void* Orig_TaskbarController_OnGroupingModeChanged = nullptr;

static LONG GetTaskbarFrameOffset() {
    static LONG taskbarFrameOffset = []() -> LONG {
        if (!Orig_TaskbarController_OnGroupingModeChanged) return 0;
#if defined(_M_X64)
        if (IsBadReadPtr(Orig_TaskbarController_OnGroupingModeChanged, 32)) return 0;
        const BYTE* p = (const BYTE*)Orig_TaskbarController_OnGroupingModeChanged;
        if (p && p[0] == 0x48 && p[1] == 0x83 && p[2] == 0xEC && (p[4] == 0x48 || p[4] == 0x4C) && p[5] == 0x8B && (p[6] & 0xC0) == 0x80) {
            LONG offset = *(LONG*)(p + 7); 
            return (offset < 0 || offset > 0xFFFF) ? 0 : offset;
        }
#endif
        return 0;
    }(); 
    return taskbarFrameOffset;
}

static void WINAPI Hook_TaskbarController_UpdateFrameHeight(void* pThis) {
    if (IsVerticalMode() || g_restoring) { 
        if (Orig_TaskbarController_UpdateFrameHeight) Orig_TaskbarController_UpdateFrameHeight(pThis); 
        return; 
    }
    LONG taskbarFrameOffset = GetTaskbarFrameOffset();
    if (!taskbarFrameOffset || IsBadReadPtr((BYTE*)pThis + taskbarFrameOffset, sizeof(void*))) { 
        if (Orig_TaskbarController_UpdateFrameHeight) Orig_TaskbarController_UpdateFrameHeight(pThis); 
        return; 
    }
    void* taskbarFrame = *(void**)((BYTE*)pThis + taskbarFrameOffset);
    if (!taskbarFrame || IsBadReadPtr(taskbarFrame, sizeof(void*)*2)) { 
        if (Orig_TaskbarController_UpdateFrameHeight) Orig_TaskbarController_UpdateFrameHeight(pThis); 
        return; 
    }
    
    FrameworkElement taskbarFrameElement = nullptr;
    IUnknown* unk = SafeGetUnk1(taskbarFrame);

    if (unk) unk->QueryInterface(winrt::guid_of<FrameworkElement>(), winrt::put_abi(taskbarFrameElement));
    
    if (!taskbarFrameElement) { 
        if (Orig_TaskbarController_UpdateFrameHeight) Orig_TaskbarController_UpdateFrameHeight(pThis); 
        return; 
    }
    
    taskbarFrameElement.MaxHeight(std::numeric_limits<double>::infinity());
    if (Orig_TaskbarController_UpdateFrameHeight) Orig_TaskbarController_UpdateFrameHeight(pThis);
    
    auto contentGrid = Media::VisualTreeHelper::GetParent(taskbarFrameElement).try_as<FrameworkElement>();
    if (contentGrid) { 
        double height = taskbarFrameElement.Height(); 
        double contentGridHeight = contentGrid.Height(); 
        if (contentGridHeight > 0 && contentGridHeight != height) contentGrid.Height(height); 
    }
}

static void WINAPI Hook_SystemTrayFrame_Height(void* pThis, double value) {
    if (!g_restoring && !IsVerticalMode() && g_inSystemTrayController_UpdateFrameSize) value = std::numeric_limits<double>::quiet_NaN();
    if (Orig_SystemTrayFrame_Height) Orig_SystemTrayFrame_Height(pThis, value);
}

static void WINAPI Hook_TaskbarFrame_Height(void* pThis, double value) {
    if (!g_restoring && !IsVerticalMode() && Orig_TaskbarFrame_MaxHeight) Orig_TaskbarFrame_MaxHeight(pThis, std::numeric_limits<double>::infinity());
    if (Orig_TaskbarFrame_Height) Orig_TaskbarFrame_Height(pThis, value);
}

struct FrameSizeScanContext { void* taskbar = nullptr; void* secondary = nullptr; void* trayStrong = nullptr; void* trayWeak = nullptr; };

static BOOL CALLBACK SymFindFrameSize(PSYMBOL_INFO pSym, ULONG, PVOID ctx) {
    auto* c = (FrameSizeScanContext*)ctx; 
    const char* n = pSym->Name;
    if (strstr(n, "dtor$") || strstr(n, "thunk") || strstr(n, "Adjustor")) return TRUE;
    
    if (strstr(n, "TaskbarConfiguration")) { if (!c->taskbar) c->taskbar = (void*)pSym->Address; } 
    else if (strstr(n, "Secondary")) { if (!c->secondary) c->secondary = (void*)pSym->Address; } 
    else { 
        if (strstr(n, "SystemTray")) { if (!c->trayStrong) c->trayStrong = (void*)pSym->Address; } 
        else { if (!c->trayWeak) c->trayWeak = (void*)pSym->Address; } 
    }
    return TRUE;
}

static std::vector<SymbolEntry> GetViewSyms(HMODULE mod) {
    return {
        {"__real@4048000000000000", (void**)&g_double48, nullptr, true, false, nullptr},
        {"GetIconHeightInViewPixels@TaskbarConfiguration@implementation@Taskbar@winrt@@SANW4", (void**)&Orig_GetIconH_Enum, (void*)Hook_GetIconH_Enum, true, false, nullptr},
        {"GetIconHeightInViewPixels@TaskbarConfiguration@implementation@Taskbar@winrt@@SANN", (void**)&Orig_GetIconH_Dbl, (void*)Hook_GetIconH_Dbl, true, false, nullptr},
        {"GetIconHeightInViewPixels@TaskbarConfiguration@implementation@Taskbar@winrt@@QEAANXZ", (void**)&Orig_GetIconH_Method, (void*)Hook_GetIconH_Method, true, false, nullptr},
        {"GetIconHeight", (void**)&Orig_GetIconH_Method, (void*)Hook_GetIconH_Method, true, false, nullptr},
        {"GetIconHeightInViewPixels", (void**)&Orig_GetIconH_Method, (void*)Hook_GetIconH_Method, true, false, nullptr},
        {"ShowAt@?$consume_Windows_UI_Xaml_Controls_Primitives_IFlyoutBase5@UMenuFlyout@", (void**)&Orig_MenuFlyout_ShowAt, (void*)Hook_MenuFlyout_ShowAt, true, false, nullptr},
        {"ShowContextMenu@TextIconContent@implementation@SystemTray@winrt", (void**)&Orig_TextIconContent_ShowContextMenu, (void*)Hook_TextIconContent_ShowContextMenu, true, false, nullptr},
        {"ShowContextMenu@DateTimeIconContent@implementation@SystemTray@winrt", (void**)&Orig_DateTimeIconContent_ShowContextMenu, (void*)Hook_DateTimeIconContent_ShowContextMenu, true, false, nullptr},
        {"UpdateFrameSize@SystemTrayController", (void**)&Orig_SystemTrayController_UpdateFrameSize, (void*)Hook_SystemTrayController_UpdateFrameSize, true, false, nullptr},
        {"UpdateFrameSize@SystemTraySecondaryController", (void**)&Orig_SystemTraySecondaryController_UpdateFrameSize, (void*)Hook_SystemTraySecondaryController_UpdateFrameSize, true, false, nullptr},
        {"Height@?$consume_Windows_UI_Xaml_IFrameworkElement@USystemTrayFrame@SystemTray@winrt", (void**)&Orig_SystemTrayFrame_Height, (void*)Hook_SystemTrayFrame_Height, true, false, nullptr},
        {"MaxHeight@?$consume_Windows_UI_Xaml_IFrameworkElement@UTaskbarFrame@implementation@Taskbar@winrt", (void**)&Orig_TaskbarFrame_MaxHeight, nullptr, true, false, nullptr},
        {"Height@?$consume_Windows_UI_Xaml_IFrameworkElement@UTaskbarFrame@implementation@Taskbar@winrt", (void**)&Orig_TaskbarFrame_Height, (void*)Hook_TaskbarFrame_Height, true, false, nullptr},
        {"UpdateFrameHeight@TaskbarController", (void**)&Orig_TaskbarController_UpdateFrameHeight, (void*)Hook_TaskbarController_UpdateFrameHeight, true, false, nullptr},
        {"OnGroupingModeChanged@TaskbarController", (void**)&Orig_TaskbarController_OnGroupingModeChanged, nullptr, true, false, nullptr},
        {"UpdateFlyoutPosition@FlyoutFrame", (void**)&Orig_FlyoutFrame_UpdateFlyoutPosition, (void*)Hook_FlyoutFrame_UpdateFlyoutPosition, true, false, nullptr},
    };
}

static bool SetupViewHooks(HMODULE mod) {
    auto syms = GetViewSyms(mod);
    if (!Resolve(mod, syms)) return false;

    FrameSizeScanContext fsc; 
    MODULEINFO mi{}; 
    GetModuleInformation(GetCurrentProcess(), mod, &mi, sizeof(mi));
    SymEnumSymbols(GetCurrentProcess(), (DWORD64)mi.lpBaseOfDll, "*GetFrameSize*", SymFindFrameSize, &fsc);
    SymEnumSymbols(GetCurrentProcess(), (DWORD64)mi.lpBaseOfDll, "*get_FrameSize*", SymFindFrameSize, &fsc); 
    
    if (!Orig_GetFrameSize && fsc.taskbar) Orig_GetFrameSize = (double(WINAPI*)(int))fsc.taskbar;
    if (!Orig_SysTray2GetFrame && fsc.secondary) Orig_SysTray2GetFrame = (double(WINAPI*)(void*,int))fsc.secondary;
    if (!Orig_SysTrayGetFrame) { 
        if (fsc.trayStrong) Orig_SysTrayGetFrame = (double(WINAPI*)(void*,int))fsc.trayStrong; 
        else if (fsc.trayWeak) Orig_SysTrayGetFrame = (double(WINAPI*)(void*,int))fsc.trayWeak; 
    }

    std::vector<DE> ds;
    for (auto& s : syms) { if (s.found && s.hookFunc && *s.ppOriginal) ds.push_back({s.ppOriginal, s.hookFunc}); }
    
    if (Orig_GetFrameSize) ds.push_back({(void**)&Orig_GetFrameSize, (void*)Hook_GetFrameSize});
    if (Orig_SysTrayGetFrame) ds.push_back({(void**)&Orig_SysTrayGetFrame, (void*)Hook_SysTrayFrame});
    if (Orig_SysTray2GetFrame) ds.push_back({(void**)&Orig_SysTray2GetFrame, (void*)Hook_SysTray2Frame});
    
    return ds.empty() || ApplyD(ds);
}

static std::vector<SymbolEntry> GetTBSyms(HMODULE mod) {
    return {
        {"GetIconSize@IconUtils", (void**)&Orig_IconUtils_GetIconSize, (void*)Hook_GetIconSize, false, false, nullptr},
        {"IsStorageRecreationRequired@IconContainer", (void**)&Orig_IconContainer_IsStorageRecreationRequired, (void*)Hook_IsStorageRecreation, false, false, nullptr},
        {"GetMinSize@TrayUI", (void**)&Orig_TrayUI_GetMinSize, (void*)Hook_GetMinSize, true, false, nullptr},
        {"GetClassLongPtrW@CIconLoadingFunctions", (void**)&Orig_CIconLoadingFunctions_GetClassLongPtrW, (void*)Hook_GetClassLongPtrW, false, false, nullptr},
        {"SendMessageCallbackW@CIconLoadingFunctions", (void**)&Orig_CIconLoadingFunctions_SendMessageCallbackW, (void*)Hook_SendMsgCbW, false, false, nullptr},
        {"_StuckTrayChange@TrayUI", (void**)&Orig_TrayUI_StuckTrayChange, nullptr, false, false, nullptr},
        {"_HandleSettingChange@TrayUI", (void**)&Orig_TrayUI_HandleSettingChange, (void*)Hook_HandleSettingChange, false, false, nullptr},
        {"GetDockedRect@TrayUI", (void**)&Orig_TrayUI_GetDockedRect, (void*)Hook_GetDockedRect, false, false, nullptr},
        {"MakeStuckRect@TrayUI", (void**)&Orig_TrayUI_MakeStuckRect, (void*)Hook_MakeStuckRect, false, false, nullptr},
        {"GetStuckInfo@TrayUI",  (void**)&Orig_TrayUI_GetStuckInfo,  (void*)Hook_GetStuckInfo,  false, false, nullptr},
        {"_ComputeJumpViewPosition@CTaskListWnd", (void**)&Orig_ComputeJumpViewPos, (void*)Hook_ComputeJumpViewPos, true, false, nullptr},
        {"DisplayUI@CTaskListThumbnailWnd", (void**)&Orig_CTaskListThumbnailWnd_DisplayUI, (void*)Hook_CTaskListThumbnailWnd_DisplayUI, true, false, nullptr},
        {"LayoutThumbnails@CTaskListThumbnailWnd", (void**)&Orig_CTaskListThumbnailWnd_LayoutThumbnails, (void*)Hook_CTaskListThumbnailWnd_LayoutThumbnails, true, false, nullptr},
    };
}

static bool SetupTBDll() {
    HMODULE mod = LoadLibraryExW(L"taskbar.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32); 
    if (!mod) return false;
    auto syms = GetTBSyms(mod);
    if (!Resolve(mod, syms)) return false;
    std::vector<DE> ds; 
    for (auto& s : syms) { if (s.found && s.hookFunc && *s.ppOriginal) ds.push_back({s.ppOriginal, s.hookFunc}); } 
    return ds.empty() || ApplyD(ds);
}

static BOOL WINAPI Hook_TrackPopupMenuEx(HMENU hMenu, UINT uFlags, int x, int y, HWND hwnd, LPTPMPARAMS lptpm) {
    if (!g_restoring && g_taskbarPos.load() == ABE_TOP) {
        HWND hTray = FindTray();
        if (hTray && (hwnd == hTray || GetAncestor(hwnd, GA_ROOT) == hTray)) {
            RECT rc;
            if (GetWindowRect(hTray, &rc) && y <= rc.bottom + 10) {
                uFlags = (uFlags & ~(TPM_BOTTOMALIGN | TPM_VCENTERALIGN)) | TPM_TOPALIGN;
                y = rc.bottom; 
            }
        }
    }
    return Orig_TrackPopupMenuEx(hMenu, uFlags, x, y, hwnd, lptpm);
}

static BOOL WINAPI Hook_SWP(HWND hWnd, HWND hAfter, int X, int Y, int cx, int cy, UINT uFlags) {
    if (g_restoring || g_taskbarPos.load() != ABE_TOP) return Orig_SWP(hWnd, hAfter, X, Y, cx, cy, uFlags);
    
    WCHAR cls[128]; 
    if (!GetClassNameW(hWnd, cls, 128)) return Orig_SWP(hWnd, hAfter, X, Y, cx, cy, uFlags);
    
    if (_wcsicmp(cls, L"TaskListThumbnailWnd") == 0) { 
        if (uFlags & SWP_NOMOVE) return Orig_SWP(hWnd, hAfter, X, Y, cx, cy, uFlags);

        HWND hTray = FindWindowW(L"Shell_TrayWnd", nullptr); 
        if (hTray) {
            if (Orig_CTaskListThumbnailWnd_DisplayUI) {
                if (!g_inCTaskListThumbnailWnd_DisplayUI && !g_inCTaskListThumbnailWnd_LayoutThumbnails) {
                    return Orig_SWP(hWnd, hAfter, X, Y, cx, cy, uFlags);
                }
                if (g_inCTaskListThumbnailWnd_DisplayUI) {
                    RECT trayRc; GetWindowRect(hTray, &trayRc);
                    Y = trayRc.bottom + MulDiv(12, GetSafeDpi(hTray), 96);
                } else {
                    RECT rc; GetWindowRect(hWnd, &rc);
                    Y = rc.top;
                }
            } else {
                RECT trayRc; GetWindowRect(hTray, &trayRc);
                int targetY = trayRc.bottom + MulDiv(12, GetSafeDpi(hTray), 96);
                if (Y < targetY) Y = targetY;
            }
        }
    } 
    else if (_wcsicmp(cls, L"TopLevelWindowForOverflowXamlIsland") == 0 || 
             _wcsicmp(cls, L"Xaml_WindowedPopupClass") == 0) {
        
        if (uFlags & (SWP_NOMOVE | SWP_NOSIZE)) return Orig_SWP(hWnd, hAfter, X, Y, cx, cy, uFlags);
        
        HWND hTray = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (hTray) {
            RECT trayRc; GetWindowRect(hTray, &trayRc);
            bool adjusted = false;

            if (_wcsicmp(cls, L"Xaml_WindowedPopupClass") == 0 &&
                GetWindowThreadProcessId(hWnd, nullptr) == GetWindowThreadProcessId(hTray, nullptr)) {
                
                HWND rootOwner = GetAncestor(hWnd, GA_ROOTOWNER);
                WCHAR rootCls[64];
                if (rootOwner && GetClassNameW(rootOwner, rootCls, 64)) {
                    if (_wcsicmp(rootCls, L"XamlExplorerHostIslandWindow") == 0) {
                        Y = trayRc.bottom + MulDiv(10 + (int)g_lastFlyoutHeight, GetSafeDpi(hTray), 96);
                        adjusted = true;
                    } else if (_wcsicmp(rootCls, L"TopLevelWindowForOverflowXamlIsland") == 0) {
                        adjusted = true;
                    }
                }
            }

            if (!adjusted) {
                if (Y < trayRc.bottom) Y = trayRc.bottom;
            }
        }
    }
    
    return Orig_SWP(hWnd, hAfter, X, Y, cx, cy, uFlags);
}

static BOOL WINAPI Hook_MW(HWND hWnd, int X, int Y, int w, int h, BOOL bRepaint) {
    if (g_restoring || g_taskbarPos.load() != ABE_TOP) return Orig_MW(hWnd, X, Y, w, h, bRepaint);
    
    WCHAR cls[128]; 
    if (GetClassNameW(hWnd, cls, 128)) {
        if (_wcsicmp(cls, L"XamlExplorerHostIslandWindow") == 0) { 
            DWORD tid = GetWindowThreadProcessId(hWnd, nullptr);
            if (tid) {
                HANDLE hThread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
                if (hThread) {
                    typedef HRESULT(WINAPI* GTD)(HANDLE, PWSTR*);
                    static GTD pGTD = (GTD)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
                    bool isMultitaskingView = false;
                    if (pGTD) { 
                        PWSTR desc = nullptr; 
                        if (SUCCEEDED(pGTD(hThread, &desc)) && desc) { 
                            isMultitaskingView = (wcscmp(desc, L"MultitaskingView") == 0); 
                            LocalFree(desc); 
                        } 
                    }
                    CloseHandle(hThread);
                    if (isMultitaskingView) { 
                        HWND hTray = FindTray(); 
                        if (hTray) { 
                            RECT trayRc; GetWindowRect(hTray, &trayRc); 
                            Y = trayRc.bottom + MulDiv(12, GetSafeDpi(hTray), 96);
                        } 
                    }
                }
            }
        }
    }
    return Orig_MW(hWnd, X, Y, w, h, bRepaint);
}

static bool SetupW32() {
    Orig_SHABM = SHAppBarMessage; 
    HMODULE hKB = GetModuleHandleW(L"kernelbase.dll"); 
    Orig_LLEW = (decltype(&LoadLibraryExW))GetProcAddress(hKB, "LoadLibraryExW");
    HMODULE hU32 = GetModuleHandleW(L"user32.dll"); 
    Orig_SWP = (decltype(&SetWindowPos))GetProcAddress(hU32, "SetWindowPos"); 
    Orig_MW = (decltype(&MoveWindow))GetProcAddress(hU32, "MoveWindow");
    Orig_MapWindowPoints = (decltype(&MapWindowPoints))GetProcAddress(hU32, "MapWindowPoints");
    Orig_TrackPopupMenuEx = (decltype(&TrackPopupMenuEx))GetProcAddress(hU32, "TrackPopupMenuEx");
    
    return ApplyD({ 
        {(void**)&Orig_SHABM, (void*)Hook_SHABM}, 
        {(void**)&Orig_LLEW, (void*)Hook_LLEW}, 
        {(void**)&Orig_SWP, (void*)Hook_SWP}, 
        {(void**)&Orig_MW, (void*)Hook_MW},
        {(void**)&Orig_MapWindowPoints, (void*)Hook_MapWindowPoints},
        {(void**)&Orig_TrackPopupMenuEx, (void*)Hook_TrackPopupMenuEx},
        {(void**)&Orig_SystemParametersInfoW, (void*)Hook_SystemParametersInfoW},
        {(void**)&Orig_GetMonitorInfoW, (void*)Hook_GetMonitorInfoW}
    });
}

static void ApplySettings(HWND hMsgHost) {
    HWND hWnd = FindTray(); 
    if (!hWnd) return;

    static bool sub = false;
    if (!sub) { SetWindowSubclass(hWnd, PosFixSubclassProc, 1, 0); sub = true; }

    g_applyingSettings = true;
    if (!IsVerticalMode()) {
        g_pendingMeasureOverride = true; 
        g_frameSizeOverrideOffset = -1.0; 
        if (!Orig_GetFrameSize && g_double48) {
            if (g_saved48 < 0) g_saved48 = *g_double48;
            double t = (double)TASKBAR_HEIGHT - 1.0;
            ProtectMemcpy(g_double48, &t, sizeof(double));
        }
        SendMessage(hWnd, WM_SETTINGCHANGE, SPI_SETLOGICALDPIOVERRIDE, 0);
        for (int i = 0; i < 20 && g_pendingMeasureOverride; i++) Sleep(10);
    }

    g_pendingMeasureOverride = true; 
    g_frameSizeOverrideOffset = 0.0; 
    if (!Orig_GetFrameSize && g_double48) {
        if (g_saved48 < 0) g_saved48 = *g_double48;
        double t = (double)TASKBAR_HEIGHT;
        ProtectMemcpy(g_double48, &t, sizeof(double));
    }
    SendMessage(hWnd, WM_SETTINGCHANGE, SPI_SETLOGICALDPIOVERRIDE, 0);
    
    if (!IsVerticalMode()) { 
        for (int i = 0; i < 20 && g_pendingMeasureOverride; i++) Sleep(10); 
    } else {
        g_pendingMeasureOverride = false;
    }
    
    RefreshTaskSw(hWnd);
    g_applyingSettings = false;
}

static void RestoreSettings() {
    UninstallJumpListEventHook();
    if (g_hSharedMem) { CloseHandle(g_hSharedMem); g_hSharedMem = nullptr; }
    if (g_double48 && g_saved48 >= 0) ProtectMemcpy(g_double48, &g_saved48, sizeof(double));
    CleanupD();
    
    HWND hWnd = FindTray();
    if (hWnd) SendMessageTimeoutW(hWnd, WM_APP + 103, 0, 0, SMTO_NORMAL, 1000, nullptr);
    HWND hMsg = FindWindowW(L"SlimBar11_MsgHost", nullptr);
    if (hMsg) DestroyWindow(hMsg);
    
    HANDLE hEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\SlimBar11_Dll_Unloaded");
    if (hEvent) { SetEvent(hEvent); CloseHandle(hEvent); }
}

static LRESULT CALLBACK MsgWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_APP + 104) {
        LoadConfig(); 
        ApplySettings(hWnd);
        return 0;
    }

    if (uMsg == g_msgSetPosition && g_msgSetPosition) {
        DWORD newPos = (DWORD)wParam; 
        g_taskbarPos = newPos;

        if (newPos == ABE_TOP) InstallJumpListEventHook();
        else UninstallJumpListEventHook();

        if (g_hSharedMem) {
            SharedConfig* cfg = (SharedConfig*)MapViewOfFile(g_hSharedMem, FILE_MAP_WRITE, 0, 0, sizeof(SharedConfig));
            if (cfg) { cfg->taskbarPos = newPos; UnmapViewOfFile(cfg); }
        }
        
        HWND hTray = FindTray();
        if (hTray) {
            HMONITOR mon = MonitorFromWindow(hTray, MONITOR_DEFAULTTONEAREST); 
            int th = GetDynamicPhysicalHeight(mon), tw = GetDynamicPhysicalWidth(mon);
            RECT rc;
            if (GetMonitorRect(mon, &rc)) {
                int x, y, w, h;
                switch (newPos) {
                    case ABE_TOP:    x = rc.left; y = rc.top; w = rc.right - rc.left; h = th; break;
                    case ABE_LEFT:   x = rc.left; y = rc.top; w = tw; h = rc.bottom - rc.top; break;
                    case ABE_RIGHT:  x = rc.right - tw; y = rc.top; w = tw; h = rc.bottom - rc.top; break;
                    default:         x = rc.left; y = rc.bottom - th; w = rc.right - rc.left; h = th; break;
                }
                SetWindowPos(hTray, 0, x, y, w, h, SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
            }
        }
        ApplySettings(hWnd); 
        return 0;
    }

    if (uMsg == g_msgRestore && g_msgRestore) {
        g_restoring = true; 
        
        UninstallJumpListEventHook();
        CleanupD();
        if (g_double48 && g_saved48 >= 0) ProtectMemcpy(g_double48, &g_saved48, sizeof(double));
        
        g_taskbarPos = ABE_BOTTOM;
        HWND hTray = FindTray();
        if (hTray) {
            g_applyingSettings = true;
            
            APPBARDATA abd = { sizeof(abd), hTray };
            SHAppBarMessage(ABM_GETTASKBARPOS, &abd); 
            abd.rc.top = abd.rc.bottom - (g_originalTaskbarHeight ? g_originalTaskbarHeight : 48);
            SHAppBarMessage(ABM_QUERYPOS, &abd);
            SHAppBarMessage(ABM_SETPOS, &abd);
            
            SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, SPI_SETWORKAREA, 0, SMTO_ABORTIFHUNG, 100, nullptr);
            SendMessage(hTray, WM_SETTINGCHANGE, 0, (LPARAM)L"TraySettings");

            HMONITOR mon = MonitorFromWindow(hTray, MONITOR_DEFAULTTONEAREST); 
            RECT rc;
            if (GetMonitorRect(mon, &rc)) {
                UINT dpiX = 96, dpiY = 96; GetDpiForMonitorSafe(mon, &dpiX, &dpiY);
                int h = MulDiv(g_originalTaskbarHeight ? g_originalTaskbarHeight : 48, dpiY, 96);
                SetWindowPos(hTray, 0, rc.left, rc.bottom - h, rc.right - rc.left, h, SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
            }
            
            RefreshTaskSw(hTray);
            g_applyingSettings = false;
        }
        
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

static BOOL WINAPI Hook_SystemParametersInfoW(UINT uiAction, UINT uiParam, PVOID pvParam, UINT fWinIni) {
    BOOL ret = Orig_SystemParametersInfoW(uiAction, uiParam, pvParam, fWinIni);
    if (uiAction == SPI_GETWORKAREA && ret && pvParam && !g_restoring && g_taskbarPos.load() == ABE_TOP) {
        RECT* rc = (RECT*)pvParam;
        if (rc->top > 0) rc->top = 0; 
    }
    return ret;
}

static BOOL WINAPI Hook_GetMonitorInfoW(HMONITOR hMonitor, LPMONITORINFO lpmi) {
    BOOL ret = Orig_GetMonitorInfoW(hMonitor, lpmi);
    if (ret && lpmi && !g_restoring && g_taskbarPos.load() == ABE_TOP) {
        if (lpmi->rcWork.top > 0) lpmi->rcWork.top = 0;
    }
    return ret;
}

static std::atomic<bool> g_unloading{false};
static HANDLE g_hWatchdogEvent = nullptr;
static HANDLE g_hWatchdogThread = nullptr;
static DWORD WINAPI WatchdogThread(LPVOID) {
    AllowAppContainerAccess();
    std::string myPath = GetModuleDir() + "\\slimbar11_hook.dll";
    while (!g_unloading) {
        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
            if (Process32FirstW(hSnap, &pe)) {
                do {
                    if (_wcsicmp(pe.szExeFile, L"SystemSettings.exe") == 0) {
                        HANDLE hProc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pe.th32ProcessID);
                        if (hProc) {
                            bool injected = false;
                            HMODULE hMods[1024]; DWORD cbNeeded;
                            if (EnumProcessModulesEx(hProc, hMods, sizeof(hMods), &cbNeeded, LIST_MODULES_ALL)) {
                                for (unsigned i = 0; i < (cbNeeded / sizeof(HMODULE)); i++) {
                                    char name[MAX_PATH];
                                    if (GetModuleBaseNameA(hProc, hMods[i], name, sizeof(name)) && _stricmp(name, "slimbar11_hook.dll") == 0) {
                                        injected = true; break;
                                    }
                                }
                            }
                            if (!injected) {
                                void* pBuf = VirtualAllocEx(hProc, NULL, MAX_PATH, MEM_COMMIT, PAGE_READWRITE);
                                if (pBuf) {
                                    WriteProcessMemory(hProc, pBuf, myPath.c_str(), myPath.length() + 1, NULL);
                                    HANDLE hLoadLibrary = (HANDLE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryA");
                                    HANDLE hThread = CreateRemoteThread(hProc, NULL, 0, (LPTHREAD_START_ROUTINE)hLoadLibrary, pBuf, 0, NULL);
                                    if (hThread) { WaitForSingleObject(hThread, 2000); CloseHandle(hThread); }
                                    VirtualFreeEx(hProc, pBuf, 0, MEM_RELEASE);
                                }
                            }
                            CloseHandle(hProc);
                        }
                    }
                } while (Process32NextW(hSnap, &pe));
            }
            CloseHandle(hSnap);
        }
        if (g_hWatchdogEvent) WaitForSingleObject(g_hWatchdogEvent, 1000); 
    }
    return 0;
}

static decltype(&SetWindowPos) Orig_SMEH_SWP = SetWindowPos;

static BOOL WINAPI Hook_SMEH_SWP(HWND hWnd, HWND hAfter, int X, int Y, int cx, int cy, UINT uFlags) {
    if (uFlags & SWP_NOMOVE) return Orig_SMEH_SWP(hWnd, hAfter, X, Y, cx, cy, uFlags);
    
    if (g_taskbarPos.load() == ABE_TOP) {
        WCHAR cls[64];
        if (GetClassNameW(hWnd, cls, 64) && _wcsicmp(cls, L"Windows.UI.Core.CoreWindow") == 0) {
            DWORD tid = GetWindowThreadProcessId(hWnd, nullptr);
            if (tid) {
                bool isJumpView = false;
                if (tid == g_jumpViewTid) {
                    isJumpView = true;
                } else {
                    HANDLE hThread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
                    if (hThread) {
                        typedef HRESULT(WINAPI* GTD)(HANDLE, PWSTR*);
                        static GTD pGTD = (GTD)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
                        if (pGTD) { 
                            PWSTR desc = nullptr; 
                            if (SUCCEEDED(pGTD(hThread, &desc)) && desc) { 
                                if (wcscmp(desc, L"JumpViewUI") == 0) {
                                    isJumpView = true;
                                    g_jumpViewTid = tid; 
                                }
                                LocalFree(desc); 
                            } 
                        }
                        CloseHandle(hThread);
                    }
                }

                if (isJumpView) { 
                    HWND hTray = FindWindowW(L"Shell_TrayWnd", nullptr); 
                    if (hTray) { 
                        RECT trayRc; GetWindowRect(hTray, &trayRc); 
                        if (trayRc.top == 0 && (trayRc.bottom - trayRc.top) < 100) Y = trayRc.bottom; 
                    } 
                }
            }
        }
    }
    return Orig_SMEH_SWP(hWnd, hAfter, X, Y, cx, cy, uFlags);
}

static void SmehCleanup() {
    DetourTransactionBegin(); 
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&)Orig_SMEH_SWP, Hook_SMEH_SWP);
    DetourTransactionCommit();
}

static DWORD WINAPI SmehInitThread(LPVOID) {
    AllowAppContainerAccess();
    
    DetourTransactionBegin(); 
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)Orig_SMEH_SWP, Hook_SMEH_SWP);
    DetourTransactionCommit();

    HANDLE hEvent = CreateEventW(NULL, TRUE, FALSE, L"Local\\SlimBar11_Uninstall_Event");
    if (hEvent) {
        WaitForSingleObject(hEvent, INFINITE); 
        SmehCleanup();
        CloseHandle(hEvent);
    }
    RestoreSettings();
    FreeLibraryAndExitThread(g_hSelfModule, 0);
    return 0;
}

static DWORD WINAPI SettingsInitThread(LPVOID) {
    AllowAppContainerAccess();
    SetupW32();
    
    HANDLE hEvent = CreateEventW(NULL, TRUE, FALSE, L"Local\\SlimBar11_Uninstall_Event");
    if (hEvent) {
        WaitForSingleObject(hEvent, INFINITE); 
        CleanupD();
        CloseHandle(hEvent);
    }
    FreeLibraryAndExitThread(g_hSelfModule, 0);
    return 0;
}

struct ResolveParam { HMODULE mod; std::vector<SymbolEntry> syms; };
static DWORD WINAPI ResolveThreadProc(LPVOID lpParam) {
    auto p = (ResolveParam*)lpParam;
    Resolve(p->mod, p->syms);
    delete p;
    return 0;
}

static DWORD WINAPI InitThread(LPVOID) {
    HMODULE hShcore = LoadLibraryW(L"shcore.dll");
    if (hShcore) g_pGetDpiForMonitor = (PFN_GETDPI)GetProcAddress(hShcore, "GetDpiForMonitor");

    g_msgSetPosition = RegisterWindowMessageW(L"SlimBar11_SetPosition");
    g_msgRestore = RegisterWindowMessageW(L"SlimBar11_Restore");
    
    #ifndef MSGFLT_ADD
    #define MSGFLT_ADD 1
    #endif
    ChangeWindowMessageFilter(g_msgSetPosition, MSGFLT_ADD); 
    ChangeWindowMessageFilter(g_msgRestore, MSGFLT_ADD);

    g_hSharedMem = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, SHARED_MEM_NAME);
    if (!g_hSharedMem) g_hSharedMem = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_NAME);
    
    if (g_hSharedMem) { 
        SharedConfig* cfg = (SharedConfig*)MapViewOfFile(g_hSharedMem, FILE_MAP_READ, 0, 0, sizeof(SharedConfig)); 
        if (cfg) { g_taskbarPos = cfg->taskbarPos; UnmapViewOfFile(cfg); } 
    }

    LoadConfig(); 

    if (g_taskbarPos.load() != ABE_BOTTOM) {
        HWND hTray = FindTray();
        if (hTray) {
            HMONITOR mon = MonitorFromWindow(hTray, MONITOR_DEFAULTTONEAREST); 
            int th = GetDynamicPhysicalHeight(mon), tw = GetDynamicPhysicalWidth(mon);
            RECT rc;
            if (GetMonitorRect(mon, &rc)) {
                int x = rc.left, y = rc.bottom - th, w = rc.right - rc.left, h = th;
                switch (g_taskbarPos.load()) {
                    case ABE_TOP:    x = rc.left; y = rc.top; w = rc.right - rc.left; h = th; break;
                    case ABE_LEFT:   x = rc.left; y = rc.top; w = tw; h = rc.bottom - rc.top; break;
                    case ABE_RIGHT:  x = rc.right - tw; y = rc.top; w = tw; h = rc.bottom - rc.top; break;
                }
                SetWindowPos(hTray, 0, x, y, w, h, SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
            }
        }
    }

    HMODULE hTB = LoadLibraryExW(L"taskbar.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE hVM = GetViewMod(false); 
    HANDLE hThreads[2];
    DWORD threadCount = 0;

    if (hTB) hThreads[threadCount++] = CreateThread(nullptr, 0, ResolveThreadProc, new ResolveParam{hTB, GetTBSyms(hTB)}, 0, nullptr);
    if (hVM) hThreads[threadCount++] = CreateThread(nullptr, 0, ResolveThreadProc, new ResolveParam{hVM, GetViewSyms(hVM)}, 0, nullptr);
    
    if (threadCount > 0) {
        WaitForMultipleObjects(threadCount, hThreads, TRUE, 30000);
        for (DWORD i = 0; i < threadCount; i++) CloseHandle(hThreads[i]);
    }

    if (!SetupTBDll()) { FreeLibraryAndExitThread(g_hSelfModule, 1); return 1; }
    
    hVM = GetViewMod(); 
    if (hVM) { g_viewLoaded = true; SetupViewHooks(hVM); }

    SetupW32();
    
    g_hWatchdogEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_hWatchdogThread = CreateThread(0, 0, WatchdogThread, 0, 0, 0);

    WNDCLASSEXW wx = { sizeof(wx), 0, MsgWndProc, 0, 0, g_hSelfModule, 0, 0, 0, 0, L"SlimBar11_MsgHost", 0 };
    RegisterClassExW(&wx);
    HWND hMsg = CreateWindowExW(0, L"SlimBar11_MsgHost", nullptr, 0, 0, 0, 0, 0, NULL, nullptr, g_hSelfModule, nullptr);
    
    ApplySettings(hMsg);

    if (g_taskbarPos.load() == ABE_TOP) {
        InstallJumpListEventHook();
    }

    HANDLE hInitEvent = CreateEventW(NULL, TRUE, FALSE, L"Local\\SlimBar11_Init_Done");
    if (hInitEvent) { SetEvent(hInitEvent); CloseHandle(hInitEvent); }
    
    if (hMsg) {
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    
    g_unloading = true;
    if (g_hWatchdogEvent) SetEvent(g_hWatchdogEvent);
    if (g_hWatchdogThread) {
        WaitForSingleObject(g_hWatchdogThread, 2000); 
        CloseHandle(g_hWatchdogThread);
    }
    if (g_hWatchdogEvent) CloseHandle(g_hWatchdogEvent);

    RestoreSettings();
    Sleep(100); 
    FreeLibraryAndExitThread(g_hSelfModule, 0);
    return 0;
}

static bool IsProcessName(const wchar_t* targetName) {
    WCHAR path[MAX_PATH]; GetModuleFileNameW(nullptr, path, MAX_PATH); 
    WCHAR* name = wcsrchr(path, L'\\'); if (name) name++; else name = path;
    return _wcsicmp(name, targetName) == 0;
}

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hSelfModule = hMod; 
        DisableThreadLibraryCalls(hMod);
        AllowAppContainerAccess(); 
        if (IsProcessName(L"StartMenuExperienceHost.exe")) {
            CloseHandle(CreateThread(0, 0, SmehInitThread, 0, 0, 0));
        } else if (IsProcessName(L"SystemSettings.exe")) {
            CloseHandle(CreateThread(0, 0, SettingsInitThread, 0, 0, 0));
        } else {
            CloseHandle(CreateThread(0, 0, InitThread, 0, 0, 0));
        }
    }
    return TRUE;
}