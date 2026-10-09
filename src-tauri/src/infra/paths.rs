//! 基础设施层 · 路径解析（执行版 §10 存储 / §14②⑥）。
//!
//! 架构强制项 ⑦：**所有库目录与素材树路径的解析都集中在这里**，其他模块不得自行拼路径。
//! P0 只做「解析」不建目录、不写文件（第一次真正落盘发生在 P1 建索引库时）。

use std::path::PathBuf;

use crate::domain::guard::Sensitive;

/// 库目录名（§10：默认位置 `%LOCALAPPDATA%\鹿铃\`）。
pub const APP_DIR_NAME: &str = "鹿铃";

fn env_path(key: &str) -> Option<PathBuf> {
    std::env::var_os(key)
        .map(PathBuf::from)
        .filter(|p| !p.as_os_str().is_empty())
}

/// 库（应用数据）目录。
///
/// §10：默认 `%LOCALAPPDATA%\鹿铃\`，可改到程序目录相对路径 `.\data\`（设置项，P8 接入）。
/// §13.4：库目录不可写时降级到用户目录并明确提示，**绝不退化到素材目录**——P0 只解析不落盘。
pub fn default_library_dir() -> PathBuf {
    if let Some(local) = env_path("LOCALAPPDATA") {
        return local.join(APP_DIR_NAME);
    }
    if let Some(profile) = env_path("USERPROFILE") {
        return profile.join("AppData").join("Local").join(APP_DIR_NAME);
    }
    // 环境变量全部缺失时（极罕见）退到进程当前目录下的 data\，由上层提示用户
    PathBuf::from("data")
}

/// 收集系统与敏感目录（§14⑥ 拒绝列表），交给核心域做纯路径判定。
pub fn sensitive_dirs(data_dir: PathBuf) -> Sensitive {
    let mut dirs: Vec<PathBuf> = Vec::new();
    for key in ["WINDIR", "SystemRoot", "ProgramFiles", "ProgramFiles(x86)", "ProgramData", "APPDATA", "LOCALAPPDATA"] {
        if let Some(p) = env_path(key) {
            dirs.push(p);
        }
    }
    if let Some(sys_drive) = env_path("SystemDrive") {
        dirs.push(sys_drive.join("$Recycle.Bin"));
    }
    Sensitive { data_dir, dirs }
}

/// 库目录（P0 只用于显示与排除判定，不创建）。
pub fn library_dir() -> PathBuf {
    default_library_dir()
}
