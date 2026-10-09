//! 基础设施层 · 卷识别（执行版 §13.2 / §13.4：索引以 `(volume_id, rel_path)` 定位）。
//!
//! 为什么需要卷序列号：素材树换盘、从别的机器复制过来时，盘符可能一样而内容完全不同；
//! 序列号能识别「还是不是同一个卷」，是「重新定位素材树」向导的第一重匹配依据（§13.4）。
//!
//! 这里直接用 `kernel32!GetVolumeInformationW`（Win32 经典签名），不额外引入 crate。

use std::path::{Path, PathBuf};

#[link(name = "kernel32")]
extern "system" {
    fn GetVolumeInformationW(
        lp_root_path_name: *const u16,
        lp_volume_name_buffer: *mut u16,
        n_volume_name_size: u32,
        lp_volume_serial_number: *mut u32,
        lp_maximum_component_length: *mut u32,
        lp_file_system_flags: *mut u32,
        lp_file_system_name_buffer: *mut u16,
        n_file_system_name_size: u32,
    ) -> i32;
}

fn wide(path: &Path) -> Vec<u16> {
    use std::os::windows::ffi::OsStrExt;
    path.as_os_str().encode_wide().chain(std::iter::once(0)).collect()
}

/// 取值形如 `D:\` 的卷挂载点；UNC 与相对路径返回 `None`。
pub fn mount_point(path: &Path) -> Option<PathBuf> {
    let text = path.to_string_lossy().replace('/', "\\");
    let bytes: Vec<char> = text.chars().collect();
    if bytes.len() < 2 || bytes[1] != ':' {
        return None;
    }
    let letter = bytes[0].to_ascii_uppercase();
    if !letter.is_ascii_alphabetic() {
        return None;
    }
    Some(PathBuf::from(format!("{letter}:\\")))
}

/// 卷序列号（十六进制，8 位）；取不到时返回 `None`。
pub fn serial_of(path: &Path) -> Option<u32> {
    let mount = mount_point(path)?;
    let w = wide(&mount);
    let mut serial: u32 = 0;
    let ok = unsafe {
        GetVolumeInformationW(
            w.as_ptr(),
            std::ptr::null_mut(),
            0,
            &mut serial,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            0,
        )
    };
    if ok == 0 || serial == 0 {
        None
    } else {
        Some(serial)
    }
}

/// 索引用的卷标识：`vol:<序列号>`，取不到序列号时退化为 `mnt:<挂载点>`。
///
/// 两者都是稳定值（同一块盘、同一挂载点不变），不会因素材树改名而变化——§13.4 正是靠它
/// 在素材树搬家后把索引重新对上。
pub fn volume_id(path: &Path) -> String {
    if let Some(serial) = serial_of(path) {
        return format!("vol:{serial:08X}");
    }
    match mount_point(path) {
        Some(mount) => format!("mnt:{}", mount.to_string_lossy().to_ascii_uppercase()),
        None => "mnt:?".to_string(),
    }
}

/// 相对卷根目录的路径（索引里的 `rel_path`）。
///
/// 用它而不是「相对素材根」的原因：同一卷上两个素材根可能含同名文件，
/// 相对素材根会撞主键；相对卷根既唯一，又能在素材树整体改名后仍用后缀匹配重定位（§13.4）。
pub fn rel_path_from_volume(path: &Path) -> String {
    let text = path.to_string_lossy().replace('/', "\\");
    let chars: Vec<char> = text.chars().collect();
    // 形如 `X:\...`：直接切掉前三字符，**保留原始大小写**（索引按 NOCASE 比较）
    if chars.len() >= 3 && chars[1] == ':' && chars[2] == '\\' {
        return chars[3..].iter().collect();
    }
    match mount_point(path) {
        Some(mount) => {
            let prefix = mount.to_string_lossy();
            let prefix = prefix.trim_end_matches('\\');
            text.strip_prefix(prefix)
                .map(|rest| rest.trim_start_matches('\\').to_string())
                .unwrap_or(text)
        }
        None => text,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn 盘符挂载点规范化() {
        assert_eq!(mount_point(Path::new(r"d:\素材\a.jpg")), Some(PathBuf::from(r"D:\")));
        assert_eq!(mount_point(Path::new("C:/x/y")), Some(PathBuf::from(r"C:\")));
        assert_eq!(mount_point(Path::new(r"\\server\share\a")), None);
        assert_eq!(mount_point(Path::new("相对路径")), None);
    }

    #[test]
    fn 相对卷根的路径() {
        assert_eq!(rel_path_from_volume(Path::new(r"D:\素材\2026\a.jpg")), r"素材\2026\a.jpg");
        assert_eq!(rel_path_from_volume(Path::new(r"d:\a.jpg")), "a.jpg");
    }

    #[test]
    fn 本机卷序列号可读且稳定() {
        let here = std::env::current_dir().expect("当前目录");
        let id1 = volume_id(&here);
        let id2 = volume_id(&here);
        assert_eq!(id1, id2, "同一路径两次取值应一致");
        assert!(id1.starts_with("vol:") || id1.starts_with("mnt:"), "意外的卷标识：{id1}");
        // 系统盘必然可读序列号（本机 Windows）
        let sys = Path::new(r"C:\");
        assert!(volume_id(sys).starts_with("vol:"), "系统盘应取到序列号");
    }
}
