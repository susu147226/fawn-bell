//! 核心域 · 工作根与敏感目录判定（纯逻辑，无 IO）。执行版 §14 ②⑥。
//!
//! 这里只做**路径规范化后的前缀比较**，不碰磁盘（存在性、可写性由基础设施层另行确认）。

use std::path::{Path, PathBuf};

/// 敏感目录集合：由基础设施层按环境变量收集后注入（核心域不读环境）。
#[derive(Debug, Clone, Default)]
pub struct Sensitive {
    /// 库目录（应用数据目录）。§14②：库目录与素材树不得重叠；§6.2：扫描强制排除它自身。
    pub data_dir: PathBuf,
    /// 系统与敏感目录（驱动器根另行由 [`is_drive_root`] 判定）。
    pub dirs: Vec<PathBuf>,
}

/// 工作根判定结论。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum RootVerdict {
    Allowed,
    /// 拒绝并把理由原样展示给用户（不静默降级）。
    Refused(String),
}

/// 规范化路径串：统一反斜杠、去掉末尾分隔符、大小写折叠（Windows 不区分大小写）。
pub fn norm(p: &Path) -> String {
    let s = p.to_string_lossy().replace('/', "\\");
    let s = s.trim_end_matches('\\').to_string();
    if s.len() == 2 && s.ends_with(':') {
        // "C:" → "C:\"，盘符根统一形态
        format!("{s}\\")
    } else {
        s.to_lowercase()
    }
}

/// 是否为盘符根（`C:\`）。§14⑥ 拒绝列表第一项。
pub fn is_drive_root(p: &Path) -> bool {
    let s = p.to_string_lossy().replace('/', "\\");
    let s = s.trim_end_matches('\\');
    s.len() == 2 && s.ends_with(':')
}

/// `child` 是否位于 `parent` 之内（含相等）。两者都按 [`norm`] 比较。
pub fn is_under(parent: &Path, child: &Path) -> bool {
    let p = norm(parent);
    let c = norm(child);
    c == p || c.starts_with(&format!("{p}\\"))
}

/// 判定一个待选工作根是否可用。拒绝的理由必须能直接展示给用户。
pub fn check_root(root: &Path, s: &Sensitive) -> RootVerdict {
    let raw = root.to_string_lossy().to_string();
    if raw.trim().is_empty() {
        return RootVerdict::Refused("没有选择目录。".into());
    }
    if is_drive_root(root) {
        return RootVerdict::Refused(format!(
            "`{raw}` 是盘符根目录。为了保护系统盘与整盘遍历性能，请改用其中的具体素材文件夹。"
        ));
    }
    if !s.data_dir.as_os_str().is_empty() {
        if is_under(&s.data_dir, root) {
            return RootVerdict::Refused(format!(
                "`{raw}` 位于鹿铃的库目录内。库目录不能作为素材树（执行版 §14②）。"
            ));
        }
    }
    for d in &s.dirs {
        if d.as_os_str().is_empty() {
            continue;
        }
        if is_under(d, root) {
            return RootVerdict::Refused(format!(
                "`{raw}` 位于系统目录 `{}` 内。执行版 §14⑥ 的拒绝列表不允许把它作为工作根。",
                d.to_string_lossy()
            ));
        }
    }
    RootVerdict::Allowed
}

/// 若库目录落在工作根之内，返回需要**强制排除**的子树（§6.2「排除应用数据目录」不可关闭）。
pub fn exclusion_for(root: &Path, data_dir: &Path) -> Option<PathBuf> {
    if data_dir.as_os_str().is_empty() {
        return None;
    }
    let r = norm(root);
    let d = norm(data_dir);
    if d.starts_with(&format!("{r}\\")) {
        Some(data_dir.to_path_buf())
    } else {
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sensitive() -> Sensitive {
        Sensitive {
            data_dir: PathBuf::from(r"C:\Users\me\AppData\Local\鹿铃"),
            dirs: vec![
                PathBuf::from(r"C:\Windows"),
                PathBuf::from(r"C:\Program Files"),
            ],
        }
    }

    #[test]
    fn 盘符根被拒() {
        let v = check_root(Path::new(r"C:\"), &sensitive());
        assert!(matches!(v, RootVerdict::Refused(_)));
        assert!(matches!(
            check_root(Path::new("D:/"), &sensitive()),
            RootVerdict::Refused(_)
        ));
    }

    #[test]
    fn 库目录内被拒() {
        let v = check_root(Path::new(r"C:\Users\me\AppData\Local\鹿铃\cache"), &sensitive());
        assert!(matches!(v, RootVerdict::Refused(_)));
        // 库目录本身
        assert!(matches!(
            check_root(Path::new(r"C:\Users\me\AppData\Local\鹿铃"), &sensitive()),
            RootVerdict::Refused(_)
        ));
    }

    #[test]
    fn 系统目录被拒() {
        assert!(matches!(
            check_root(Path::new(r"C:\Windows\System32"), &sensitive()),
            RootVerdict::Refused(_)
        ));
        assert!(matches!(
            check_root(Path::new(r"c:\program files\某软件"), &sensitive()),
            RootVerdict::Refused(_)
        ));
    }

    #[test]
    fn 正常素材目录放行() {
        assert_eq!(
            check_root(Path::new(r"D:\素材\2026"), &sensitive()),
            RootVerdict::Allowed
        );
        // 与库目录同名前缀但不在其下
        assert_eq!(
            check_root(Path::new(r"C:\Users\me\AppData\Local\鹿铃备份"), &sensitive()),
            RootVerdict::Allowed
        );
    }

    #[test]
    fn 库目录在根内时被排除() {
        let root = Path::new(r"C:\Users\me\AppData\Local");
        let ex = exclusion_for(root, &sensitive().data_dir);
        assert_eq!(ex, Some(PathBuf::from(r"C:\Users\me\AppData\Local\鹿铃")));
        assert_eq!(exclusion_for(Path::new(r"D:\素材"), &sensitive().data_dir), None);
    }

    #[test]
    fn 包含关系判定() {
        assert!(is_under(Path::new(r"D:\素材"), Path::new(r"D:\素材\a\b")));
        assert!(is_under(Path::new(r"D:\素材"), Path::new(r"D:\素材")));
        assert!(!is_under(Path::new(r"D:\素材"), Path::new(r"D:\素材备份")));
    }
}
