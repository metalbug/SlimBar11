use anyhow::{Context, Result};
use goblin::pe::PE;
use indicatif::{ProgressBar, ProgressStyle};
use pdb::{FallibleIterator, PDB};
use pelite::pe64::{Pe, PeFile};
use std::collections::{HashMap, HashSet};
use std::fs::{self, File};
use std::io::{Read, Write};
use std::path::Path;

// 定义我们需要查找的符号配置
fn get_target_symbols() -> HashMap<&'static str, Vec<&'static str>> {
    let mut m = HashMap::new();

    // Taskbar.dll 的目标符号
    m.insert(
        "taskbar.dll",
        vec![
            "GetIconSize@IconUtils",
            "IsStorageRecreationRequired@IconContainer",
            "GetMinSize@TrayUI",
            "GetClassLongPtrW@CIconLoadingFunctions",
            "SendMessageCallbackW@CIconLoadingFunctions",
            "_StuckTrayChange@TrayUI",
            "_HandleSettingChange@TrayUI",
            "GetDockedRect@TrayUI",
            "MakeStuckRect@TrayUI",
            "GetStuckInfo@TrayUI",
            "_ComputeJumpViewPosition@CTaskListWnd",
            
            // 新增：GDI 老版应用缩略图引擎支持
            "DisplayUI@CTaskListThumbnailWnd",
            "LayoutThumbnails@CTaskListThumbnailWnd",
        ],
    );

    // Taskbar.View.dll 的目标符号
    m.insert(
        "taskbar.view.dll",
        vec![
            "__real@4048000000000000",
            "GetIconHeightInViewPixels@TaskbarConfiguration@implementation@Taskbar@winrt@@SANW4",
            "GetIconHeightInViewPixels@TaskbarConfiguration@implementation@Taskbar@winrt@@SANN",
            // 候选列表 (优先级: QEAANXZ > GetIconHeightInViewPixels > GetIconHeight)
            "GetIconHeightInViewPixels@TaskbarConfiguration@implementation@Taskbar@winrt@@QEAANXZ",
            "GetIconHeightInViewPixels",
            "GetIconHeight",
            "ShowAt@?$consume_Windows_UI_Xaml_Controls_Primitives_IFlyoutBase5@UMenuFlyout@",
            "ShowContextMenu@TextIconContent@implementation@SystemTray@winrt",
            "ShowContextMenu@DateTimeIconContent@implementation@SystemTray@winrt",
            "UpdateFrameSize@SystemTrayController",
            "UpdateFrameSize@SystemTraySecondaryController",
            "Height@?$consume_Windows_UI_Xaml_IFrameworkElement@USystemTrayFrame@SystemTray@winrt",
            "MaxHeight@?$consume_Windows_UI_Xaml_IFrameworkElement@UTaskbarFrame@implementation@Taskbar@winrt",
            "Height@?$consume_Windows_UI_Xaml_IFrameworkElement@UTaskbarFrame@implementation@Taskbar@winrt",
            "UpdateFrameHeight@TaskbarController",
            "OnGroupingModeChanged@TaskbarController",
            
            // 新增：新版 24H2 XAML 悬浮缩略图定位函数
            "UpdateFlyoutPosition@FlyoutFrame",
            
            // FrameSize 相关候选 (从 SymFindFrameSize 逻辑派生)
            "GetFrameSize@TaskbarConfiguration",
            "GetFrameSize@SystemTray",
            "GetFrameSize@Secondary",
            "get_FrameSize",
        ],
    );
    m
}

struct DllInfo {
    original_filename: String,
    version: String,
    pdb_name: String,
    pdb_signature: String, // PDB_GUID + Age
}

fn main() -> Result<()> {
    let dll_source = Path::new("dll_source");
    let pdb_cache = Path::new("pdb_cache");
    let output_file = Path::new("offsets.ini");

    // 1. 初始化目录
    if !dll_source.exists() {
        fs::create_dir_all(dll_source)?;
        println!("请将 taskbar.dll / .blob 文件放入 'dll_source' 文件夹中。");
        return Ok(());
    }
    fs::create_dir_all(pdb_cache)?;

    let target_map = get_target_symbols();

    // 获取文件列表
    let entries: Vec<_> = walkdir::WalkDir::new(dll_source)
        .into_iter()
        .filter_map(|e| e.ok())
        .filter(|e| e.path().is_file())
        .collect();

    println!("扫描到 {} 个文件，开始分析...", entries.len());

    let main_pb = ProgressBar::new(entries.len() as u64);
    main_pb.set_style(
        ProgressStyle::default_bar().template(
            "{spinner:.green} [{elapsed_precise}] [{bar:40.cyan/blue}] {pos}/{len} {msg}",
        )?,
    );

    struct OutputEntry {
        version: String,
        section_header: String,
        offsets: Vec<String>,
    }
    let mut results: Vec<OutputEntry> = Vec::new();

    for entry in entries {
        let path = entry.path();
        let display_name = path.file_name().unwrap().to_string_lossy();

        main_pb.set_message(format!("分析文件: {}", display_name));

        // 2. 解析 DLL 信息
        let dll_info = match parse_dll_info(path) {
            Ok(info) => info,
            Err(_) => {
                main_pb.inc(1);
                continue;
            }
        };

        let target_key = dll_info.original_filename.to_lowercase();
        if !target_map.contains_key(target_key.as_str()) {
            main_pb.inc(1);
            continue;
        }

        // 使用 GUID 重命名本地 PDB，防止不同版本互相覆盖
        let unique_pdb_name = format!("{}_{}", dll_info.pdb_name, dll_info.pdb_signature);
        let pdb_path = pdb_cache.join(&unique_pdb_name);

        if !pdb_path.exists() {
            main_pb.suspend(|| {
                println!(
                    "正在下载符号: {} (版本: {})",
                    unique_pdb_name, dll_info.version
                );
                if let Err(e) = download_pdb_with_progress(&dll_info, &pdb_path) {
                    eprintln!("  下载失败: {}", e);
                }
            });

            if !pdb_path.exists() || pdb_path.metadata().map(|m| m.len() == 0).unwrap_or(true) {
                main_pb.inc(1);
                continue;
            }
        }

        // 4. 解析 PDB
        let targets = target_map.get(target_key.as_str()).unwrap();
        match extract_offsets(&pdb_path, targets) {
            Ok(mut offsets) => {
                // === 核心逻辑：合并 GetIconHeight 的多个候选 ===
                if target_key == "taskbar.view.dll" {
                    let key_qea = "GetIconHeightInViewPixels@TaskbarConfiguration@implementation@Taskbar@winrt@@QEAANXZ";
                    let key_short1 = "GetIconHeightInViewPixels";
                    let key_short2 = "GetIconHeight";

                    let mut best_rva = None;

                    // 优先级 1: QEAANXZ
                    if let Some(pos) = offsets.iter().position(|(k, _)| k == key_qea) {
                        best_rva = Some(offsets[pos].1);
                        offsets.remove(pos); // 移除原始 key
                    }

                    // 优先级 2: GetIconHeightInViewPixels
                    if best_rva.is_none() {
                        if let Some(pos) = offsets.iter().position(|(k, _)| k == key_short1) {
                            best_rva = Some(offsets[pos].1);
                            offsets.remove(pos);
                        }
                    } else {
                        // 已找到更优，删除低优
                        offsets.retain(|(k, _)| k != key_short1);
                    }

                    // 优先级 3: GetIconHeight
                    if best_rva.is_none() {
                        if let Some(pos) = offsets.iter().position(|(k, _)| k == key_short2) {
                            best_rva = Some(offsets[pos].1);
                            offsets.remove(pos);
                        }
                    } else {
                        offsets.retain(|(k, _)| k != key_short2);
                    }

                    // 如果找到了任何一个，添加统一的 GetIconHeight
                    if let Some(rva) = best_rva {
                        offsets.push(("GetIconHeight".to_string(), rva));
                    } else {
                        // 如果三个都不存在，直接舍弃这个 PDB
                        main_pb.suspend(|| {
                            eprintln!(
                                "  [跳过] PDB 缺少关键符号 (GetIconHeight): {}",
                                dll_info.pdb_name
                            );
                        });
                        continue;
                    }
                }

                let header = format!("[{}_{}]", dll_info.pdb_name, dll_info.pdb_signature);
                let mut lines = Vec::new();

                // 排序以保持稳定输出
                offsets.sort_by(|a, b| a.0.cmp(&b.0));

                for (key, rva) in offsets {
                    lines.push(format!("{}={:X}", key, rva));
                }

                results.push(OutputEntry {
                    version: dll_info.version,
                    section_header: header,
                    offsets: lines,
                });
            }
            Err(e) => {
                main_pb.suspend(|| eprintln!("  PDB 解析失败 {:?}: {}", pdb_path, e));
            }
        }

        main_pb.inc(1);
    }

    main_pb.finish_with_message("所有任务完成");

    // 5. 写入结果
    results.sort_by(|a, b| a.version.cmp(&b.version));

    let mut current_version = String::new();
    let mut file = File::create(output_file)?;

    for entry in results {
        if entry.version != current_version {
            writeln!(file, "\n// {}", entry.version)?;
            current_version = entry.version.clone();
        }

        writeln!(file, "{}", entry.section_header)?;
        for line in entry.offsets {
            writeln!(file, "{}", line)?;
        }
    }

    println!("结果已保存到 {:?}", output_file);
    Ok(())
}

fn parse_dll_info(path: &Path) -> Result<DllInfo> {
    let mut file = File::open(path)?;
    let mut buffer = Vec::new();
    file.read_to_end(&mut buffer)?;

    // Goblin: 获取 PDB 信息
    let pe = PE::parse(&buffer)?;
    let debug_data = pe.debug_data.context("No debug data found")?;
    let codeview = debug_data
        .codeview_pdb70_debug_info
        .context("No CodeView data")?;

    // === 核心修复：手动调整 GUID 字节序 ===
    // PE 文件中的 GUID 结构体: { Data1(4), Data2(2), Data3(2), Data4(8) }
    // Data1, Data2, Data3 是小端序存储，需要反转。Data4 是数组，保持原样。
    let s = codeview.signature; // [u8; 16]
    let guid_hex = format!(
        "{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}",
        s[3], s[2], s[1], s[0], // Data1 (4 bytes) - Reverse
        s[5], s[4],             // Data2 (2 bytes) - Reverse
        s[7], s[6],             // Data3 (2 bytes) - Reverse
        s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15] // Data4 (8 bytes) - Keep
    );

    let age = codeview.age;

    let pdb_name = std::str::from_utf8(codeview.filename)?
        .trim_matches(char::from(0))
        .to_string();

    let pdb_signature = format!("{}{:X}", guid_hex, age);

    // Pelite: 获取版本和原始文件名
    let pe_file = PeFile::from_bytes(&buffer)?;
    let resources = pe_file.resources()?;
    let version_info = resources.version_info()?;

    let fixed = version_info.fixed().context("No fixed version info")?;
    let version_str = fixed.dwFileVersion.to_string();
    let parts: Vec<&str> = version_str.split('.').collect();
    let short_version = if parts.len() >= 4 {
        format!("{}.{}", parts[2], parts[3])
    } else {
        version_str
    };

    let mut original_filename = String::from("unknown");
    let translations = version_info.translation();
    for &lang in translations {
        if let Some(name) = version_info.value(lang, "OriginalFilename") {
            if !name.is_empty() {
                original_filename = name;
                break;
            }
        }
    }

    Ok(DllInfo {
        original_filename,
        version: short_version,
        pdb_name,
        pdb_signature,
    })
}

fn download_pdb_with_progress(info: &DllInfo, save_path: &Path) -> Result<()> {
    let download_url = format!(
        "https://msdl.microsoft.com/download/symbols/{}/{}/{}",
        info.pdb_name, info.pdb_signature, info.pdb_name
    );

    let client = reqwest::blocking::Client::builder()
        .user_agent("Microsoft-Symbol-Server/10.0.10036.206")
        .build()?;

    let mut response = client.get(&download_url).send()?;

    if !response.status().is_success() {
        return Err(anyhow::anyhow!("服务器返回错误: {}", response.status()));
    }

    let total_size = response.content_length().unwrap_or(0);

    let pb = ProgressBar::new(total_size);
    pb.set_style(ProgressStyle::default_bar()
        .template("{spinner:.green} [{elapsed_precise}] [{bar:40.cyan/blue}] {bytes}/{total_bytes} ({bytes_per_sec}, {eta})")?
        .progress_chars("#>-"));

    let mut file = File::create(save_path)?;
    let mut buffer = [0; 8192];

    loop {
        let bytes_read = response.read(&mut buffer)?;
        if bytes_read == 0 {
            break;
        }
        file.write_all(&buffer[..bytes_read])?;
        pb.inc(bytes_read as u64);
    }

    pb.finish_with_message("下载完成");
    Ok(())
}

fn extract_offsets(pdb_path: &Path, targets: &[&str]) -> Result<Vec<(String, u32)>> {
    let file = File::open(pdb_path)?;
    let mut pdb = PDB::open(file)?;

    let symbol_table = pdb.global_symbols()?;
    let address_map = pdb.address_map()?;

    let mut found_offsets = Vec::new();
    let mut found_keys = HashSet::new();

    println!("\n------ 分析 PDB: {:?} ------", pdb_path);

    let mut symbols = symbol_table.iter();
    while let Some(symbol) = symbols.next()? {
        match symbol.parse() {
            Ok(pdb::SymbolData::Public(data)) => {
                let name = data.name.to_string();
                let params = (name, data.offset);
                process_symbol(
                    params,
                    &address_map,
                    targets,
                    &mut found_keys,
                    &mut found_offsets,
                );
            }
            Ok(pdb::SymbolData::Procedure(data)) => {
                let name = data.name.to_string();
                let params = (name, data.offset);
                process_symbol(
                    params,
                    &address_map,
                    targets,
                    &mut found_keys,
                    &mut found_offsets,
                );
            }
            _ => {}
        }
    }
    println!("------------------------------------\n");

    let mut sorted_results = Vec::new();
    for &target in targets {
        if let Some((_, rva)) = found_offsets.iter().find(|(k, _)| k == target) {
            sorted_results.push((target.to_string(), *rva));
        }
    }

    Ok(sorted_results)
}

fn process_symbol<'a>(
    (name, offset): (std::borrow::Cow<str>, pdb::PdbInternalSectionOffset),
    address_map: &pdb::AddressMap,
    targets: &[&'a str],
    found_keys: &mut HashSet<&'a str>,
    found_offsets: &mut Vec<(String, u32)>,
) {
    let name_str = name.to_string();

    // === 1. 标准匹配 ===
    for &target in targets {
        // 对于短名，强制使用精确匹配，防止包含匹配 (如 TaskbarConfiguration::GetIconHeight)
        let is_match = if target == "GetIconHeight" || target == "GetIconHeightInViewPixels" {
            name_str == target
        } else {
            name_str.contains(target)
        };

        if is_match && !found_keys.contains(target) {
            if let Some(rva) = offset.to_rva(address_map) {
                found_offsets.push((target.to_string(), rva.0));
                found_keys.insert(target);
            }
        }
    }
}