// ============================================================
// SlimBar11 Injector
// ============================================================

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <shlobj.h>
#include <taskschd.h>
#include <comdef.h>
#include <sddl.h>
#include <string>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "comsupp.lib")

const char* DLL_NAME = "slimbar11_hook.dll";
const wchar_t* SHARED_MEM_NAME = L"Local\\SlimBar11_Config_Mem";
struct SharedConfig { DWORD taskbarPos; };

static bool EnableDebugPrivilege() {
    HANDLE hToken;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) return false;
    LUID luid;
    if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &luid)) { CloseHandle(hToken); return false; }
    TOKEN_PRIVILEGES tp{}; 
    tp.PrivilegeCount = 1; 
    tp.Privileges[0].Luid = luid; 
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool ok = AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    CloseHandle(hToken);
    return ok && GetLastError() == ERROR_SUCCESS;
}

static bool CreateLowIntegritySA(SECURITY_ATTRIBUTES& sa, PSECURITY_DESCRIPTOR& pSD) {
    LPCWSTR szSD = L"D:(A;OICI;GRGW;;;WD)(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)S:(ML;;NW;;;LW)";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(szSD, SDDL_REVISION_1, &pSD, NULL)) return false;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES); 
    sa.lpSecurityDescriptor = pSD; 
    sa.bInheritHandle = FALSE;
    return true;
}

static DWORD FindProcessId(const std::wstring& name) {
    PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return 0;
    DWORD pid = 0;
    if (Process32FirstW(hSnap, &pe)) { 
        do { if (_wcsicmp(pe.szExeFile, name.c_str()) == 0) { pid = pe.th32ProcessID; break; } } while (Process32NextW(hSnap, &pe)); 
    }
    CloseHandle(hSnap);
    return pid;
}

static bool InjectDll(DWORD pid, const char* dllPath) {
    HANDLE hProc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid); 
    if (!hProc) return false;
    void* pBuf = VirtualAllocEx(hProc, NULL, MAX_PATH, MEM_COMMIT, PAGE_READWRITE);
    if (!pBuf) { CloseHandle(hProc); return false; }
    if (!WriteProcessMemory(hProc, pBuf, dllPath, strlen(dllPath) + 1, NULL)) { VirtualFreeEx(hProc, pBuf, 0, MEM_RELEASE); CloseHandle(hProc); return false; }
    HANDLE hThread = CreateRemoteThread(hProc, NULL, 0, (LPTHREAD_START_ROUTINE)LoadLibraryA, pBuf, 0, NULL);
    if (!hThread) { VirtualFreeEx(hProc, pBuf, 0, MEM_RELEASE); CloseHandle(hProc); return false; }
    WaitForSingleObject(hThread, INFINITE);
    VirtualFreeEx(hProc, pBuf, 0, MEM_RELEASE); 
    CloseHandle(hThread); CloseHandle(hProc);
    return true;
}

static DWORD WaitForExplorer() {
    for (int i = 0; i < 300; ++i) { 
        HWND hTray = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (hTray) {
            DWORD pid = 0;
            GetWindowThreadProcessId(hTray, &pid);
            if (pid) return pid;
        }
        Sleep(100); 
    }
    return 0;
}

static bool IsExplorerJustStarted(DWORD pid) {
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return false;
    FILETIME ftCreate, ftExit, ftKernel, ftUser, ftNow;
    bool justStarted = false;
    if (GetProcessTimes(hProc, &ftCreate, &ftExit, &ftKernel, &ftUser)) {
        GetSystemTimeAsFileTime(&ftNow);
        ULARGE_INTEGER create, now;
        create.LowPart = ftCreate.dwLowDateTime;
        create.HighPart = ftCreate.dwHighDateTime;
        now.LowPart = ftNow.dwLowDateTime;
        now.HighPart = ftNow.dwHighDateTime;
        ULONGLONG diffMs = (now.QuadPart - create.QuadPart) / 10000;
        if (diffMs < 10000) justStarted = true;
    }
    CloseHandle(hProc);
    return justStarted;
}

static bool IsRunAsAdmin() {
    BOOL fIsRunAsAdmin = FALSE;
    PSID pAdministratorsGroup = NULL;
    SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &pAdministratorsGroup)) {
        CheckTokenMembership(NULL, pAdministratorsGroup, &fIsRunAsAdmin);
        FreeSid(pAdministratorsGroup);
    }
    return fIsRunAsAdmin;
}

static bool SetupAutoStart(DWORD taskbarPos) {
    if (!IsRunAsAdmin()) return false;
    wchar_t exePath[MAX_PATH]; 
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    
    std::wstring args = L"";
    if (taskbarPos == ABE_TOP) args += L"-t ";
    else if (taskbarPos == ABE_LEFT) args += L"-l ";
    else if (taskbarPos == ABE_RIGHT) args += L"-r ";

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED); 
    if (FAILED(hr)) return false;
    ITaskService* pSvc = NULL;
    hr = CoCreateInstance(CLSID_TaskScheduler, NULL, CLSCTX_INPROC_SERVER, IID_ITaskService, (void**)&pSvc);
    if (SUCCEEDED(hr)) {
        hr = pSvc->Connect(VARIANT(), VARIANT(), VARIANT(), VARIANT());
        ITaskFolder* pFolder = NULL;
        if (SUCCEEDED(hr)) hr = pSvc->GetFolder(_bstr_t(L"\\"), &pFolder);
        if (SUCCEEDED(hr)) {
            pFolder->DeleteTask(_bstr_t(L"SlimBar11AutoStart"), 0);
            ITaskDefinition* pDef = NULL; 
            hr = pSvc->NewTask(0, &pDef);
            if (SUCCEEDED(hr)) {
                IPrincipal* pPrinc = NULL; 
                pDef->get_Principal(&pPrinc);
                pPrinc->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN); 
                pPrinc->put_RunLevel(TASK_RUNLEVEL_HIGHEST); 
                pPrinc->Release();
                
                ITriggerCollection* pTrigs = NULL; pDef->get_Triggers(&pTrigs);
                ITrigger* pTrig = NULL; pTrigs->Create(TASK_TRIGGER_LOGON, &pTrig); 
                pTrig->Release(); pTrigs->Release();

                IActionCollection* pActs = NULL; pDef->get_Actions(&pActs);
                IAction* pAct = NULL; pActs->Create(TASK_ACTION_EXEC, &pAct);
                IExecAction* pExec = NULL; pAct->QueryInterface(IID_IExecAction, (void**)&pExec);
                pExec->put_Path(_bstr_t(exePath));
                pExec->put_Arguments(_bstr_t(args.c_str()));
                pExec->Release(); pAct->Release(); pActs->Release();

                ITaskSettings* pSet = NULL; pDef->get_Settings(&pSet);
                pSet->put_DisallowStartIfOnBatteries(VARIANT_FALSE); 
                pSet->put_StopIfGoingOnBatteries(VARIANT_FALSE);
                pSet->put_ExecutionTimeLimit(_bstr_t(L"PT0S")); 
                pSet->Release();

                IRegisteredTask* pReg = NULL;
                pFolder->RegisterTaskDefinition(_bstr_t(L"SlimBar11AutoStart"), pDef, TASK_CREATE_OR_UPDATE, VARIANT(), VARIANT(), TASK_LOGON_INTERACTIVE_TOKEN, VARIANT(), &pReg);
                if (pReg) pReg->Release(); 
                pDef->Release();
            }
            pFolder->Release();
        }
        pSvc->Release();
    }
    CoUninitialize();
    return true;
}

static bool RemoveAutoStart() {
    if (!IsRunAsAdmin()) return false;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED); 
    if (FAILED(hr)) return false;
    ITaskService* pSvc = NULL;
    hr = CoCreateInstance(CLSID_TaskScheduler, NULL, CLSCTX_INPROC_SERVER, IID_ITaskService, (void**)&pSvc);
    if (SUCCEEDED(hr)) {
        hr = pSvc->Connect(VARIANT(), VARIANT(), VARIANT(), VARIANT());
        ITaskFolder* pFolder = NULL;
        if (SUCCEEDED(hr)) {
            hr = pSvc->GetFolder(_bstr_t(L"\\"), &pFolder);
            if (SUCCEEDED(hr)) {
                pFolder->DeleteTask(_bstr_t(L"SlimBar11AutoStart"), 0);
                pFolder->Release();
            }
        }
        pSvc->Release();
    }
    CoUninitialize();
    return true;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    EnableDebugPrivilege();

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    DWORD taskbarPos = ABE_BOTTOM;
    bool uninstall = false;
    
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            if (_wcsicmp(argv[i], L"-t") == 0 || _wcsicmp(argv[i], L"--top") == 0) taskbarPos = ABE_TOP;
            else if (_wcsicmp(argv[i], L"-l") == 0 || _wcsicmp(argv[i], L"--left") == 0) taskbarPos = ABE_LEFT;
            else if (_wcsicmp(argv[i], L"-r") == 0 || _wcsicmp(argv[i], L"--right") == 0) taskbarPos = ABE_RIGHT;
            else if (_wcsicmp(argv[i], L"-u") == 0 || _wcsicmp(argv[i], L"--uninstall") == 0) uninstall = true;
        }
    }
    LocalFree(argv);

    HWND hMsgHost = FindWindowW(L"SlimBar11_MsgHost", nullptr);

    if (uninstall) {
        HANDLE hSmehEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\SlimBar11_Uninstall_Event");
        if (hSmehEvent) { SetEvent(hSmehEvent); CloseHandle(hSmehEvent); }

        HANDLE hUnloadEvent = CreateEventW(NULL, TRUE, FALSE, L"Local\\SlimBar11_Dll_Unloaded");

        if (hMsgHost) {
            UINT msgRestore = RegisterWindowMessageW(L"SlimBar11_Restore");
            SendMessageTimeoutW(hMsgHost, msgRestore, 0, 0, SMTO_NORMAL, 2000, nullptr);
        }

        if (hUnloadEvent) {
            WaitForSingleObject(hUnloadEvent, 2000);
            CloseHandle(hUnloadEvent);
        }
        RemoveAutoStart();
        return 0;
    }

    SECURITY_ATTRIBUTES sa; 
    PSECURITY_DESCRIPTOR pSD = nullptr;
    HANDLE hMap = nullptr;
    if (CreateLowIntegritySA(sa, pSD)) {
        hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(SharedConfig), SHARED_MEM_NAME);
        if (!hMap && GetLastError() == ERROR_ACCESS_DENIED) hMap = OpenFileMappingW(FILE_MAP_WRITE, FALSE, SHARED_MEM_NAME);
        if (hMap) {
            SharedConfig* p = (SharedConfig*)MapViewOfFile(hMap, FILE_MAP_WRITE, 0, 0, sizeof(SharedConfig));
            if (p) { p->taskbarPos = taskbarPos; UnmapViewOfFile(p); }
        }
        LocalFree(pSD);
    }

    SetupAutoStart(taskbarPos);

    if (hMsgHost) {
        UINT msgPos = RegisterWindowMessageW(L"SlimBar11_SetPosition");
        SendMessageTimeoutW(hMsgHost, msgPos, taskbarPos, 0, SMTO_NORMAL, 2000, nullptr);
        if (hMap) CloseHandle(hMap);
        return 0;
    }

    char dir[MAX_PATH]; 
    GetModuleFileNameA(NULL, dir, MAX_PATH);
    char* slash = strrchr(dir, '\\'); 
    if (slash) *slash = '\0';
    std::string dllPath = std::string(dir) + "\\" + DLL_NAME;
    
    if (GetFileAttributesA(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) return 1;

    DWORD pid = WaitForExplorer();
    if (pid) {
        if (IsExplorerJustStarted(pid)) Sleep(2000); 

        HANDLE hInitEvent = CreateEventW(NULL, TRUE, FALSE, L"Local\\SlimBar11_Init_Done");
        if (InjectDll(pid, dllPath.c_str())) {
            if (hInitEvent) WaitForSingleObject(hInitEvent, 5000);
            
            for (int i = 0; i < 20; i++) {
                hMsgHost = FindWindowW(L"SlimBar11_MsgHost", nullptr);
                if (hMsgHost) break;
                Sleep(50);
            }

            if (hMsgHost) {
                UINT msgPos = RegisterWindowMessageW(L"SlimBar11_SetPosition");
                SendMessageTimeoutW(hMsgHost, msgPos, taskbarPos, 0, SMTO_NORMAL, 2000, nullptr);
            }
        }
        if (hInitEvent) CloseHandle(hInitEvent);
    }

    DWORD smehPid = FindProcessId(L"StartMenuExperienceHost.exe");
    if (smehPid) InjectDll(smehPid, dllPath.c_str());

    if (hMap) CloseHandle(hMap); 
    return 0;
}