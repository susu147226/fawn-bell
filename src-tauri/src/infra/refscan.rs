//! 基础设施层 · 引用映射解析（执行版 §6.3.1 / §7.1）。
//!
//! §7.1 要求引用映射**在扫描期与索引同批建立**，不额外遍历素材树；§6.3.1 限定为
//! 「仅文本级文件名匹配」——解析同目录与上溯目录中的 `*.xml` / `*.json` / `*.txt` /
//! `*.js` / `*.css` / `*.html`，把里面出现的文件名记下来（含行号）。
//!
//! 这里不做语法解析、不做递归解析、不猜：只认「带扩展名的 token」。映射缺失时上层
//! 退化为不提示（§16 第 20 条④）。

/// 允许作为「引用持有者」的扩展名（§6.3.1 明文清单）。
pub const HOLDER_EXTS: [&str; 6] = ["xml", "json", "txt", "js", "css", "html"];

/// 单个持有者文件的读取上限（避免把巨大文本全读进内存）。
pub const MAX_HOLDER_BYTES: u64 = 4 * 1024 * 1024;

/// 单个 token 的长度上限（文件名不会这么长）。
const MAX_TOKEN: usize = 120;

/// 是否是需要解析引用映射的文本文件。
pub fn is_holder(ext: &str) -> bool {
    let e = ext.to_ascii_lowercase();
    HOLDER_EXTS.contains(&e.as_str())
}

fn is_token_char(c: char) -> bool {
    c.is_ascii_alphanumeric()
        || matches!(c, '_' | '-' | '.' | '+' | '@')
        || ('\u{4e00}'..='\u{9fff}').contains(&c)
}

/// 从文本中抽取「形如文件名」的 token，附带行号（1 起）。
///
/// 规则：至少含一个 `.`、扩展名是 1–6 位 ASCII 字母数字、总长 ≤ [`MAX_TOKEN`]。
/// 返回 (小写文件名, 行号)，按出现顺序；重复项保留（行号不同即不同引用）。
pub fn extract_refs(text: &str) -> Vec<(String, i64)> {
    let mut out: Vec<(String, i64)> = Vec::new();
    let mut token = String::new();
    let mut line: i64 = 1;

    let flush = |token: &mut String, line: i64, out: &mut Vec<(String, i64)>| {
        if token.is_empty() {
            return;
        }
        let t = token.as_str();
        if t.len() <= MAX_TOKEN {
            if let Some((stem, ext)) = t.rsplit_once('.') {
                let ok_ext = !ext.is_empty()
                    && ext.len() <= 6
                    && ext.chars().all(|c| c.is_ascii_alphanumeric())
                    && ext.chars().any(|c| c.is_ascii_alphabetic());
                let ok_stem = !stem.is_empty() && stem != "." && stem != "..";
                if ok_ext && ok_stem {
                    out.push((t.to_lowercase(), line));
                }
            }
        }
        token.clear();
    };

    for c in text.chars() {
        if c == '\n' {
            flush(&mut token, line, &mut out);
            line += 1;
            continue;
        }
        if is_token_char(c) {
            token.push(c);
        } else {
            flush(&mut token, line, &mut out);
        }
    }
    flush(&mut token, line, &mut out);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn 持有者扩展名判定() {
        assert!(is_holder("xml"));
        assert!(is_holder("JSON"));
        assert!(is_holder("html"));
        assert!(!is_holder("png"));
        assert!(!is_holder(""));
    }

    #[test]
    fn 抽取带扩展名的文件名与行号() {
        let text = "<root>\n  <img src=\"bj.png\"/>\n  <img src='xt_01.jpg'/>\n</root>\n";
        let refs = extract_refs(text);
        assert_eq!(
            refs,
            vec![("bj.png".to_string(), 2), ("xt_01.jpg".to_string(), 3)]
        );
    }

    #[test]
    fn 中文文件名_路径_版本号等也能认() {
        let text = "封面.png 说明.docx ../../assets/底图.PSD v1.2 build.min.js\n";
        let names: Vec<String> = extract_refs(text).into_iter().map(|(n, _)| n).collect();
        assert!(names.contains(&"封面.png".to_string()));
        assert!(names.contains(&"说明.docx".to_string()));
        assert!(names.contains(&"底图.psd".to_string()));
        assert!(names.contains(&"build.min.js".to_string()));
        // 纯版本号 v1.2 的扩展名是数字 → 不算文件名
        assert!(!names.contains(&"v1.2".to_string()));
    }

    #[test]
    fn 重复引用按行号分别记() {
        let refs = extract_refs("a.png\na.png\n");
        assert_eq!(refs.len(), 2);
        assert_eq!(refs[0].1, 1);
        assert_eq!(refs[1].1, 2);
    }

    #[test]
    fn 超长token被丢弃() {
        let long = format!("{}.png", "x".repeat(200));
        assert!(extract_refs(&long).is_empty());
    }
}
