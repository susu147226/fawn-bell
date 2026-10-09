//! 核心域 · 默认忽略规则（纯逻辑，无 IO）。执行版 §6.2。
//!
//! 「用户自建导出目录」与可编辑的忽略规则集属 P1（清单落 JSON / 库内），P0 只固化 §6.2 的默认集。

/// 默认忽略的文件名（大小写不敏感比较）。
pub const IGNORED_NAMES: &[&str] = &["Thumbs.db", ".DS_Store", "desktop.ini"];

/// 判断某个目录项是否应在扫描时跳过；`hidden_attr` 由基础设施层读到的 Windows 隐藏属性位传入。
pub fn is_ignored(name: &str, hidden_attr: bool) -> bool {
    ignore_reason(name, hidden_attr).is_some()
}

/// 给出被忽略的原因（用于界面解释「为什么看不到这个文件」，也为 P1 的设置页预留）。
pub fn ignore_reason(name: &str, hidden_attr: bool) -> Option<&'static str> {
    if hidden_attr {
        return Some("隐藏属性");
    }
    if name.starts_with('.') {
        return Some("点开头");
    }
    if name.starts_with("~$") {
        return Some("Office 临时文件");
    }
    // 按**字节**比较后缀，不能用 `name[name.len() - 4..]` 做字符串切片：
    // 中文名（如「子目录」9 字节）在 len()-4 处不是字符边界，会 panic。
    let bytes = name.as_bytes();
    if bytes.len() > 4 && bytes[bytes.len() - 4..].eq_ignore_ascii_case(b".tmp") {
        return Some("临时文件");
    }
    if IGNORED_NAMES
        .iter()
        .any(|n| n.eq_ignore_ascii_case(name))
    {
        return Some("系统占位文件");
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn 默认忽略集() {
        for n in ["Thumbs.db", "thumbs.DB", ".DS_Store", "desktop.ini", "Desktop.Ini"] {
            assert!(is_ignored(n, false), "{n} 应被忽略");
        }
        assert!(is_ignored(".git", false));
        assert!(is_ignored("~$doc.docx", false));
        assert!(is_ignored("下载.tmp", false));
        assert!(is_ignored("正常文件.jpg", true), "隐藏属性优先");
    }

    #[test]
    fn 正常文件不被忽略() {
        for n in ["DSC_0001.JPG", "note.txt", "a.tmpx", "tmp", "素材.psd"] {
            assert!(!is_ignored(n, false), "{n} 不应被忽略");
        }
    }

    #[test]
    fn 中文名不触发字节切片() {
        // 回归：曾用 name[name.len() - 4..] 做后缀比较，中文名会 panic（非字符边界）
        for n in ["子目录", "保留", "素材", "一二三四五六七八九十"] {
            assert!(!is_ignored(n, false), "{n} 不应被忽略");
            assert_eq!(ignore_reason(n, false), None);
        }
        assert!(is_ignored("备份.tmp", false));
        assert!(is_ignored("备份.TMP", false));
        assert!(!is_ignored("备份.tmp2", false));
    }

    #[test]
    fn 原因可解释() {
        assert_eq!(ignore_reason("Thumbs.db", false), Some("系统占位文件"));
        assert_eq!(ignore_reason(".hidden", false), Some("点开头"));
        assert_eq!(ignore_reason("a.tmp", false), Some("临时文件"));
        assert_eq!(ignore_reason("normal.png", false), None);
    }
}
