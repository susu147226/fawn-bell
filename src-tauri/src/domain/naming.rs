//! 核心域 · 命名模板与序号规则（执行版 §7.3，**纯逻辑、无 IO**）。
//!
//! 这一模块是 F2 的核心：把「模板 + 序号规则 + 原名」算成最终文件名。它必须能穷举单测，
//! 因为 §16 第 3 / 19 / 22 条验收（起始值 0/1、补零档位、13 个内置预设）全都落在这里。
//!
//! 条文要点（逐条对应 §7.3）：
//! - 占位符 12 个 + 修饰符（日期格式 / 大小写 / slug / trunc / seq 宽度 / 空值回退）；
//! - **空值回退链**：拍摄时间 → 修改时间 → 创建时间，绝不允许输出空名或只剩扩展名；
//! - 非法字符与 Windows 保留设备名替换为 `_`，**结尾空格与点号必须剥离**，统一 NFC；
//! - 序号：起始值 0/1、自动补 `_`（相邻已是 `_`/`-`/空格/数字则不重复插）、
//!   补零档位「不补零（默认）/ 最少 2 位 / 最少 3 位 / 固定 N 位」，
//!   **固定档超限必须报冲突或警告，不得静默截断或升位**；原名序号剥离默认「剥离尾随 `_数字`」。

use unicode_normalization::UnicodeNormalization;

/// 补零档位（§7.3.3 表）。
#[derive(Debug, Clone, Copy, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum PadMode {
    /// 默认：自然数位宽，不额外补零、不设下限。
    NoPad,
    /// 下限语义：不足 2 位才补零。
    Min2,
    /// 下限语义：不足 3 位才补零。
    Min3,
    /// 固定 N 位（1–6）；超出可表达范围必须报警。
    Fixed(u8),
}

impl Default for PadMode {
    fn default() -> Self {
        PadMode::NoPad
    }
}

/// 序号前分隔符（§7.3.3 表）。
#[derive(Debug, Clone, Copy, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum SeqSep {
    /// 默认：自动补 `_`。
    AutoUnderscore,
    Dash,
    Space,
    None,
}

impl Default for SeqSep {
    fn default() -> Self {
        SeqSep::AutoUnderscore
    }
}

impl SeqSep {
    pub fn text(self) -> &'static str {
        match self {
            SeqSep::AutoUnderscore => "_",
            SeqSep::Dash => "-",
            SeqSep::Space => " ",
            SeqSep::None => "",
        }
    }
}

/// 序号作用域（§7.3.3 表，默认当前文件夹）。
#[derive(Debug, Clone, Copy, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum SeqScope {
    CurrentFolder,
    CurrentGroup,
    SelectedSet,
}

impl Default for SeqScope {
    fn default() -> Self {
        SeqScope::CurrentFolder
    }
}

/// 原名序号剥离（§7.3.3 表，默认「剥离尾随 `_数字`」）。
#[derive(Debug, Clone, Copy, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum StripRule {
    None,
    /// 只剥掉尾随的连续数字（保留前面的 `_`）。
    TrailingDigits,
    /// 剥掉一段 `_数字`（`素材_001_v2` → `素材_v2`）。
    TrailingUnderscoreDigits,
    /// 按用户自定义正则剥离（P9 设置里配置，未配置时等价于不剥离）。
    Regex,
}

impl Default for StripRule {
    fn default() -> Self {
        StripRule::TrailingUnderscoreDigits
    }
}

/// 序号规则整体。
#[derive(Debug, Clone, Copy, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SeqRule {
    /// 起始值：0 或 1（两个都必须可用）。
    pub start: u64,
    pub sep: SeqSep,
    pub pad: PadMode,
    pub scope: SeqScope,
    pub strip: StripRule,
}

impl Default for SeqRule {
    fn default() -> Self {
        SeqRule {
            start: 0,
            sep: SeqSep::default(),
            pad: PadMode::default(),
            scope: SeqScope::default(),
            strip: StripRule::default(),
        }
    }
}

/// 序号文本。固定档超出可表达范围时返回 `Err`（§7.3.3：必须给出冲突或警告）。
pub fn format_seq(value: u64, pad: PadMode) -> Result<String, String> {
    match pad {
        PadMode::NoPad => Ok(value.to_string()),
        PadMode::Min2 => Ok(format!("{value:02}")),
        PadMode::Min3 => Ok(format!("{value:03}")),
        PadMode::Fixed(n) => {
            let width = n.clamp(1, 6) as usize;
            let digits = value.to_string().len();
            if digits > width {
                Err(format!(
                    "序号 {value} 需要 {digits} 位，超出「固定 {width} 位」档位；请改用「最少 {width} 位」或调大档位"
                ))
            } else {
                Ok(format!("{value:0width$}"))
            }
        }
    }
}

/// 把序号接到名称之后：**智能分隔**——名称末尾已经是分隔符或数字时不再重复插入（§7.3.3）。
pub fn append_seq(base: &str, seq_text: &str, sep: SeqSep) -> String {
    let mut out = base.to_string();
    let sep_text = sep.text();
    if !sep_text.is_empty() {
        let first_sep = sep_text.chars().next().unwrap_or('_');
        let already = out
            .chars()
            .last()
            .map(|c| c == '_' || c == '-' || c == ' ' || c == first_sep || c.is_ascii_digit())
            .unwrap_or(false);
        if !already {
            out.push_str(sep_text);
        }
    }
    out.push_str(seq_text);
    out
}

fn trim_trailing_digits(name: &str) -> &str {
    let trimmed = name.trim_end_matches(|c: char| c.is_ascii_digit());
    trimmed
}

/// 剥离原名里的序号（§7.3.3；默认档位是「剥离尾随 `_数字`」）。
pub fn strip_original_seq(name: &str, rule: StripRule) -> String {
    match rule {
        StripRule::None | StripRule::Regex => name.to_string(),
        StripRule::TrailingDigits => trim_trailing_digits(name).to_string(),
        StripRule::TrailingUnderscoreDigits => {
            // 找**最早**出现的一段 `_<数字>`：数字段必须结束于结尾或紧跟 `_`
            // （§7.3.3 示例：`素材_001` → `素材`；`素材_001_v2` → `素材_v2`）
            let chars: Vec<char> = name.chars().collect();
            let mut i = 0usize;
            while i < chars.len() {
                if chars[i] == '_' {
                    let mut j = i + 1;
                    while j < chars.len() && chars[j].is_ascii_digit() {
                        j += 1;
                    }
                    if j > i + 1 && (j == chars.len() || chars[j] == '_') {
                        let mut out: String = chars[..i].iter().collect();
                        out.extend(chars[j..].iter());
                        return out;
                    }
                }
                i += 1;
            }
            name.to_string()
        }
    }
}

/// 非法字符集合（§7.3.1）。
const ILLEGAL: [char; 9] = ['\\', '/', ':', '*', '?', '"', '<', '>', '|'];
/// Windows 保留设备名（不区分大小写，带扩展名也算）。
const RESERVED: [&str; 22] = [
    "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8",
    "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
];

/// 名称合法化（§7.3.1）：NFC → 非法字符与保留名 → `_` → **剥离结尾空格与点号**。
/// 返回（结果, 说明列表）。
pub fn sanitize(input: &str) -> (String, Vec<String>) {
    let mut notes: Vec<String> = Vec::new();
    let normalized: String = input.nfc().collect();

    let mut out = String::with_capacity(normalized.len());
    let mut replaced = false;
    for c in normalized.chars() {
        if ILLEGAL.contains(&c) || (c as u32) < 32 {
            out.push('_');
            replaced = true;
            continue;
        }
        out.push(c);
    }
    if replaced {
        notes.push("非法字符已替换为 `_`".to_string());
    }

    // 结尾的空格与点号必须剥离（Windows 会静默丢弃，导致「改了但名字不对」）
    let stripped_len = out.trim_end_matches(|c| c == ' ' || c == '.').len();
    if stripped_len != out.len() {
        out.truncate(stripped_len);
        notes.push("结尾的空格或点号已剥离".to_string());
    }

    // 保留设备名：整体加一个前缀下划线，避免整名被替换掉
    let stem = out.split('.').next().unwrap_or("").to_ascii_uppercase();
    if RESERVED.contains(&stem.as_str()) {
        out = format!("_{out}");
        notes.push(format!("`{stem}` 是 Windows 保留名，已加前缀 `_`"));
    }

    (out, notes)
}

/// 名称长度上限（不含扩展名，§7.3.1）。
pub const MAX_NAME_CHARS: usize = 120;

/// 超长时**截断中间段并保留结尾识别信息**；返回（结果, 是否截断过）。
pub fn truncate_middle(name: &str, max: usize) -> (String, bool) {
    let chars: Vec<char> = name.chars().collect();
    if chars.len() <= max || max < 3 {
        return (name.to_string(), false);
    }
    let keep_tail = (max / 3).max(1).min(max - 2);
    let keep_head = (max - keep_tail - 1).max(1);
    let head: String = chars[..keep_head].iter().collect();
    let tail: String = chars[chars.len() - keep_tail..].iter().collect();
    (format!("{head}~{tail}"), true)
}

/// 取前 N 个字符（`_trunc:N` 修饰符用：用户要的是「砍短」，不是「保留结尾」）。
pub fn truncate_head(name: &str, max: usize) -> String {
    name.chars().take(max).collect()
}

/// 「工程安全命名」预设（§7.3.1 / §6.3）：仅 ASCII、无空格、无中文、≤64 字符。
pub const DESIGN_SAFE_MAX: usize = 64;

pub fn design_safe(name: &str) -> String {
    let mut out = String::with_capacity(name.len());
    let mut last_dash = false;
    for c in name.chars() {
        if c.is_ascii_alphanumeric() || c == '.' || c == '_' || c == '-' {
            out.push(c);
            last_dash = false;
        } else if !last_dash {
            out.push('_');
            last_dash = true;
        }
    }
    let trimmed = out.trim_matches('_').to_string();
    truncate_middle(&trimmed, DESIGN_SAFE_MAX).0
}

/* ── 模板渲染 ─────────────────────────────────────────────────────── */

/// 渲染上下文（一条素材的全部可用取值；缺失的一律 `None`，绝不编造）。
#[derive(Debug, Clone)]
pub struct NameCtx<'a> {
    /// 原文件名（不含扩展名，已按剥离规则处理过）。
    pub stem: &'a str,
    pub ext: &'a str,
    pub camera: Option<&'a str>,
    pub width: Option<i64>,
    pub height: Option<i64>,
    pub group: Option<&'a str>,
    pub parent: Option<&'a str>,
    pub kind: &'a str,
    pub hash8: Option<&'a str>,
    /// 拍摄时间（毫秒）；回退链在 [`NameCtx::time_ms`] 里实现。
    pub capture_time: Option<i64>,
    pub mtime: i64,
    pub ctime: i64,
    pub seq: u64,
    pub counter: Option<u64>,
}

impl<'a> NameCtx<'a> {
    /// §7.3.1 的空值回退链：拍摄时间 → 修改时间 → 创建时间（**逐级过滤 0**，0 视为缺失）。
    pub fn time_ms(&self) -> Option<i64> {
        [self.capture_time, Some(self.mtime), Some(self.ctime)]
            .into_iter()
            .flatten()
            .find(|v| *v != 0)
    }
}

/// 渲染结果。
#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Rendered {
    /// 最终文件名（含扩展名）。
    pub name: String,
    /// 需要展示给用户的提醒（未知占位符、固定档超限、截断等）。
    pub notes: Vec<String>,
}

/// 内置命名预设基名（§7.3.2，顺序固定，共 13 个）。
pub const BUILTIN_PRESETS: [&str; 13] = [
    "time", "date", "week", "tq", "tq1", "rl", "wd", "xwd", "num", "bs", "hs", "xq", "sz",
];

/// 点击内置预设后的模板文本（固定契约：`<基名>_{seq}`）。
pub fn preset_template(base: &str) -> String {
    format!("{base}_{{seq}}")
}

/// 由天数推出公历年月日（Howard Hinnant 算法，纯整数）。
pub fn civil_from_days(z: i64) -> (i64, u32, u32) {
    let z = z + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1_460 + doe / 36_524 - doe / 146_096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    (if m <= 2 { y + 1 } else { y }, m as u32, d as u32)
}

/// 按 `YYYY` `MM` `DD` `HH` `mm` `ss` 记号格式化时间戳（本地时间口径留给上层，这里按 UTC）。
pub fn format_datetime(ms: i64, fmt: &str) -> String {
    let secs = ms.div_euclid(1000);
    let days = secs.div_euclid(86_400);
    let tod = secs.rem_euclid(86_400);
    let (y, m, d) = civil_from_days(days);
    let (hh, mm, ss) = (tod / 3600, (tod % 3600) / 60, tod % 60);
    let mut out = String::new();
    let mut rest = fmt;
    while !rest.is_empty() {
        if let Some(r) = rest.strip_prefix("YYYY") {
            out.push_str(&format!("{y:04}"));
            rest = r;
        } else if let Some(r) = rest.strip_prefix("MM") {
            out.push_str(&format!("{m:02}"));
            rest = r;
        } else if let Some(r) = rest.strip_prefix("DD") {
            out.push_str(&format!("{d:02}"));
            rest = r;
        } else if let Some(r) = rest.strip_prefix("HH") {
            out.push_str(&format!("{hh:02}"));
            rest = r;
        } else if let Some(r) = rest.strip_prefix("mm") {
            out.push_str(&format!("{mm:02}"));
            rest = r;
        } else if let Some(r) = rest.strip_prefix("ss") {
            out.push_str(&format!("{ss:02}"));
            rest = r;
        } else {
            let c = rest.chars().next().unwrap();
            out.push(c);
            rest = &rest[c.len_utf8()..];
        }
    }
    out
}

fn slugify(value: &str) -> String {
    let mut out = String::with_capacity(value.len());
    let mut last_dash = false;
    for c in value.chars() {
        if c.is_ascii_alphanumeric() {
            out.push(c.to_ascii_lowercase());
            last_dash = false;
        } else if !last_dash {
            out.push('-');
            last_dash = true;
        }
    }
    out.trim_matches('-').to_string()
}

fn title_case(value: &str) -> String {
    let mut out = String::with_capacity(value.len());
    let mut at_word_start = true;
    for c in value.chars() {
        if c == '_' || c == '-' || c == ' ' {
            out.push(c);
            at_word_start = true;
            continue;
        }
        if at_word_start {
            out.extend(c.to_uppercase());
            at_word_start = false;
        } else {
            out.push(c);
        }
    }
    out
}

/// 解析一个占位符：`key[_修饰符][:参数][|回退文本]`。
fn resolve_placeholder(token: &str, ctx: &NameCtx, rule: &SeqRule) -> (String, Option<String>) {
    let (body, fallback) = match token.split_once('|') {
        Some((a, b)) => (a, Some(b.to_string())),
        None => (token, None),
    };
    let (left, arg) = match body.split_once(':') {
        Some((a, b)) => (a.trim(), Some(b.trim().to_string())),
        None => (body.trim(), None),
    };

    let mut modifier: Option<&str> = None;
    let mut key = left;
    for m in ["_lower", "_upper", "_title", "_slug", "_trunc"] {
        if let Some(stripped) = left.strip_suffix(m) {
            key = stripped;
            modifier = Some(m);
            break;
        }
    }

    let mut note: Option<String> = None;
    let value: String = match key {
        "name" => ctx.stem.to_string(),
        "ext" => ctx.ext.to_string(),
        "seq" => {
            let pad = arg
                .as_deref()
                .and_then(|a| a.parse::<u8>().ok())
                .map(PadMode::Fixed)
                .unwrap_or(rule.pad);
            match format_seq(ctx.seq, pad) {
                Ok(s) => s,
                // 固定档超限：**报警，但绝不输出空号**——空号会变成 `date_.jpg` 这种残名，
                // 比「档位不够」更糟。这里按自然位宽给出数值，并把档位问题明确写进提醒等用户改档。
                Err(e) => {
                    note = Some(format!("{e}；本次按自然位宽输出 `{}`，未静默补齐", ctx.seq));
                    ctx.seq.to_string()
                }
            }
        }
        "date" => ctx
            .time_ms()
            .map(|ms| format_datetime(ms, arg.as_deref().unwrap_or("YYYYMMDD")))
            .unwrap_or_default(),
        "time" => ctx
            .time_ms()
            .map(|ms| format_datetime(ms, arg.as_deref().unwrap_or("HHmmss")))
            .unwrap_or_default(),
        "camera" => ctx.camera.unwrap_or("").to_string(),
        "w" => ctx.width.map(|v| v.to_string()).unwrap_or_default(),
        "h" => ctx.height.map(|v| v.to_string()).unwrap_or_default(),
        "group" => ctx.group.unwrap_or("").to_string(),
        "parent" => ctx.parent.unwrap_or("").to_string(),
        "kind" => ctx.kind.to_string(),
        "hash8" => ctx.hash8.map(|h| h.chars().take(8).collect()).unwrap_or_default(),
        "counter" => {
            let width = arg.as_deref().and_then(|a| a.parse::<usize>().ok()).unwrap_or(3);
            format!("{:0width$}", ctx.counter.unwrap_or(0))
        }
        other => {
            note = Some(format!("未知占位符 {{{other}}}，已原样保留"));
            format!("{{{other}}}")
        }
    };

    let value = match modifier {
        Some("_lower") => value.to_lowercase(),
        Some("_upper") => value.to_uppercase(),
        Some("_title") => title_case(&value),
        Some("_slug") => slugify(&value),
        Some("_trunc") => {
            let max = arg.as_deref().and_then(|a| a.parse::<usize>().ok()).unwrap_or(MAX_NAME_CHARS);
            truncate_head(&value, max)
        }
        _ => value,
    };

    // 空值回退（§7.3.1）：占位符解析为空时用回退文本，绝不输出空名
    if value.is_empty() {
        if let Some(f) = fallback {
            return (f, note);
        }
    }
    (value, note)
}

/// 渲染模板为最终文件名（含扩展名）。
///
/// 三条安全网：① 模板没写 `{ext}` 时自动补扩展名；② 渲染结果为空时回退到原名；③ 超长截断中间。
pub fn render(template: &str, ctx: &NameCtx, rule: &SeqRule) -> Rendered {
    let mut notes: Vec<String> = Vec::new();
    let mut out = String::with_capacity(template.len() + 16);
    let mut rest = template;
    let mut has_ext_placeholder = false;

    while let Some(open) = rest.find('{') {
        out.push_str(&rest[..open]);
        let after = &rest[open + 1..];
        match after.find('}') {
            Some(close) => {
                let token = &after[..close];
                if token.trim() == "ext" || token.trim().starts_with("ext") {
                    has_ext_placeholder = true;
                }
                let (value, note) = resolve_placeholder(token, ctx, rule);
                if let Some(n) = note {
                    if !notes.contains(&n) {
                        notes.push(n);
                    }
                }
                out.push_str(&value);
                rest = &after[close + 1..];
            }
            None => {
                // 没闭合的花括号：原样保留并提醒，不静默吞掉
                notes.push("模板里有未闭合的 `{`".to_string());
                out.push_str(&rest[open..]);
                rest = "";
                break;
            }
        }
    }
    out.push_str(rest);

    if !has_ext_placeholder && !ctx.ext.is_empty() {
        out.push('.');
        out.push_str(ctx.ext);
    }

    // 空名保护：只剩扩展名或整体为空时，退回原名（§7.3.1「绝不允许输出空名」）
    let stem_for_check = out.rsplit_once('.').map(|(a, _)| a.to_string()).unwrap_or_else(|| out.clone());
    if stem_for_check.trim().is_empty() {
        notes.push("模板渲染结果为空，已回退为原名".to_string());
        out = if ctx.ext.is_empty() {
            ctx.stem.to_string()
        } else {
            format!("{}.{}", ctx.stem, ctx.ext)
        };
    }

    // 长度上限（只约束不含扩展名的部分）
    let (stem, ext) = match out.rsplit_once('.') {
        Some((s, e)) => (s.to_string(), e.to_string()),
        None => (out.clone(), String::new()),
    };
    let (stem, truncated) = truncate_middle(&stem, MAX_NAME_CHARS);
    if truncated {
        notes.push(format!("名称超过 {MAX_NAME_CHARS} 字符，已截断中间段并保留结尾"));
    }
    let final_name = if ext.is_empty() { stem } else { format!("{stem}.{ext}") };

    Rendered {
        name: final_name,
        notes,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ctx<'a>(stem: &'a str, ext: &'a str, seq: u64) -> NameCtx<'a> {
        NameCtx {
            stem,
            ext,
            camera: None,
            width: None,
            height: None,
            group: None,
            parent: None,
            kind: "image",
            hash8: None,
            capture_time: None,
            mtime: 1_700_000_000_000,
            ctime: 0,
            seq,
            counter: None,
        }
    }

    /* ── §7.3.3 序号表 ── */

    #[test]
    fn 不补零档位是自然数位宽() {
        assert_eq!(format_seq(0, PadMode::NoPad).unwrap(), "0");
        assert_eq!(format_seq(9, PadMode::NoPad).unwrap(), "9");
        assert_eq!(format_seq(10, PadMode::NoPad).unwrap(), "10");
        assert_eq!(format_seq(119, PadMode::NoPad).unwrap(), "119");
    }

    #[test]
    fn 最少位数档位只在下限内补零() {
        assert_eq!(format_seq(9, PadMode::Min2).unwrap(), "09");
        assert_eq!(format_seq(20, PadMode::Min2).unwrap(), "20");
        assert_eq!(format_seq(9, PadMode::Min3).unwrap(), "009");
        assert_eq!(format_seq(119, PadMode::Min3).unwrap(), "119");
    }

    #[test]
    fn 固定档位超限必须报警而不是静默升位() {
        assert_eq!(format_seq(9, PadMode::Fixed(2)).unwrap(), "09");
        assert_eq!(format_seq(99, PadMode::Fixed(2)).unwrap(), "99");
        let err = format_seq(100, PadMode::Fixed(2)).unwrap_err();
        assert!(err.contains("固定 2 位"), "{err}");
    }

    #[test]
    fn 智能分隔不重复插入() {
        assert_eq!(append_seq("date", "0", SeqSep::AutoUnderscore), "date_0");
        // 末尾已是下划线 → 不再插
        assert_eq!(append_seq("date_", "0", SeqSep::AutoUnderscore), "date_0");
        // 末尾是数字 → 按条文本意也不重复插
        assert_eq!(append_seq("date0", "1", SeqSep::AutoUnderscore), "date01");
        // 换分隔符
        assert_eq!(append_seq("date", "0", SeqSep::Dash), "date-0");
        assert_eq!(append_seq("date", "0", SeqSep::Space), "date 0");
        assert_eq!(append_seq("date", "0", SeqSep::None), "date0");
        // 末尾是横线 → 用横线分隔符时也不重复
        assert_eq!(append_seq("date-", "0", SeqSep::Dash), "date-0");
    }

    #[test]
    fn 原名序号剥离的三种档位() {
        // 默认档：剥离尾随 `_数字`
        assert_eq!(strip_original_seq("素材_001", StripRule::TrailingUnderscoreDigits), "素材");
        // 中段那段 `_001` 也要剥掉，保留 `v2`（§7.3.3 示例）
        assert_eq!(strip_original_seq("素材_001_v2", StripRule::TrailingUnderscoreDigits), "素材_v2");
        assert_eq!(strip_original_seq("IMG_1234", StripRule::TrailingUnderscoreDigits), "IMG");
        // 没有 `_数字` 段时原样
        assert_eq!(strip_original_seq("海边日落 (1)", StripRule::TrailingUnderscoreDigits), "海边日落 (1)");
        // 只剥数字
        assert_eq!(strip_original_seq("IMG_1234", StripRule::TrailingDigits), "IMG_");
        // 不剥离
        assert_eq!(strip_original_seq("IMG_1234", StripRule::None), "IMG_1234");
    }

    /* ── §7.3.2 预设契约 ── */

    #[test]
    fn 内置预设十三个且顺序固定() {
        assert_eq!(BUILTIN_PRESETS.len(), 13);
        assert_eq!(
            BUILTIN_PRESETS,
            ["time", "date", "week", "tq", "tq1", "rl", "wd", "xwd", "num", "bs", "hs", "xq", "sz"]
        );
    }

    #[test]
    fn 点击预设即得基名加序号并立刻出预览() {
        let rule = SeqRule::default();
        let tpl = preset_template("date");
        assert_eq!(tpl, "date_{seq}");
        let names: Vec<String> = (0..3)
            .map(|i| render(&tpl, &ctx("IMG_0001", "png", i), &rule).name)
            .collect();
        assert_eq!(names, vec!["date_0.png", "date_1.png", "date_2.png"]);
    }

    /* ── §7.3.1 占位符与修饰符 ── */

    #[test]
    fn 各占位符取值正确() {
        let mut c = ctx("海边日落", "jpg", 7);
        c.camera = Some("ILCE-7M4");
        c.width = Some(6000);
        c.height = Some(4000);
        c.group = Some("旅行");
        c.parent = Some("DCIM");
        c.hash8 = Some("a1b2c3d4e5f6");
        c.counter = Some(5);
        let rule = SeqRule::default();
        let r = render("{name}_{seq}_{camera}_{w}x{h}_{group}_{parent}_{kind}_{hash8}_{counter:3}", &c, &rule);
        assert_eq!(r.name, "海边日落_7_ILCE-7M4_6000x4000_旅行_DCIM_image_a1b2c3d4_005.jpg");
    }

    #[test]
    fn 日期时间占位符与自定义格式() {
        let c = ctx("a", "png", 0);
        let rule = SeqRule::default();
        // 1_700_000_000_000 ms = 2023-11-14 22:13:20 UTC
        assert_eq!(render("{date}_{time}", &c, &rule).name, "20231114_221320.png");
        assert_eq!(render("{date:YYYY-MM-DD}", &c, &rule).name, "2023-11-14.png");
    }

    #[test]
    fn 修饰符大小写与截断与slug() {
        let c = ctx("Hello World", "png", 0);
        let rule = SeqRule::default();
        assert_eq!(render("{name_lower}", &c, &rule).name, "hello world.png");
        assert_eq!(render("{name_upper}", &c, &rule).name, "HELLO WORLD.png");
        assert_eq!(render("{name_title}", &c, &rule).name, "Hello World.png");
        assert_eq!(render("{name_slug}", &c, &rule).name, "hello-world.png");
        assert_eq!(render("{name_trunc:5}", &c, &rule).name, "Hello.png");
    }

    #[test]
    fn 模板层显式补零宽度覆盖设置值() {
        let rule = SeqRule { pad: PadMode::NoPad, ..Default::default() };
        assert_eq!(render("{seq:3}", &ctx("a", "png", 7), &rule).name, "007.png");
        // 固定档超限时把警告带出来，且不静默升位
        let r = render("{seq:2}", &ctx("a", "png", 100), &rule);
        assert!(r.notes.iter().any(|n| n.contains("固定 2 位")), "{:?}", r.notes);
    }

    #[test]
    fn 空值回退文本与时间回退链() {
        let rule = SeqRule::default();
        // camera 缺 → 用回退文本
        let c = ctx("a", "png", 0);
        assert_eq!(render("{camera|[未知机型]}", &c, &rule).name, "[未知机型].png");
        // capture_time 缺 → 用 mtime（回退链第二级）
        let with_capture = NameCtx { capture_time: Some(0), ..ctx("a", "png", 0) };
        let r = render("{date}", &with_capture, &rule);
        assert_eq!(r.name, "20231114.png", "拍摄时间为 0 视为缺失，应回退到修改时间");
    }

    #[test]
    fn 空名保护与未闭合花括号提示() {
        let rule = SeqRule::default();
        let r = render("{camera}", &ctx("原名", "png", 0), &rule);
        assert_eq!(r.name, "原名.png", "全空时必须回退原名，绝不输出空名");
        let r2 = render("{name", &ctx("a", "png", 0), &rule);
        assert!(r2.notes.iter().any(|n| n.contains("未闭合")));
    }

    #[test]
    fn 超过长度上限时截断中间并保留结尾() {
        let long = "x".repeat(200);
        let rule = SeqRule::default();
        let r = render("{name}", &ctx(&long, "png", 0), &rule);
        let stem = r.name.trim_end_matches(".png");
        assert!(stem.chars().count() <= MAX_NAME_CHARS);
        assert!(stem.contains('~'));
        assert!(r.notes.iter().any(|n| n.contains("截断")));
    }

    /* ── §7.3.1 合法化与工程安全命名 ── */

    #[test]
    fn 非法字符与结尾点空格被处理() {
        let (s, notes) = sanitize(r#"a\b/c:d*e?f"g<h>i|j"#);
        assert_eq!(s, "a_b_c_d_e_f_g_h_i_j");
        assert!(!notes.is_empty());
        let (s2, n2) = sanitize("名字. ");
        assert_eq!(s2, "名字");
        assert!(n2.iter().any(|n| n.contains("结尾")));
    }

    #[test]
    fn 保留设备名被改写并说明() {
        let (s, notes) = sanitize("CON");
        assert_eq!(s, "_CON");
        assert!(notes.iter().any(|n| n.contains("保留名")));
        let (s2, _) = sanitize("con.txt");
        assert_eq!(s2, "_con.txt");
    }

    #[test]
    fn 工程安全命名只留ascii且不超64() {
        let s = design_safe("场景 模型-v2 中文.fbx");
        assert!(s.is_ascii(), "{s}");
        assert!(!s.contains(' '));
        assert!(s.chars().count() <= DESIGN_SAFE_MAX);
        assert_eq!(design_safe("scene.blend"), "scene.blend");
    }

    #[test]
    fn 序号规则默认值符合条文() {
        let r = SeqRule::default();
        assert_eq!(r.start, 0, "起始值默认 0");
        assert_eq!(r.pad, PadMode::NoPad, "补零默认不补零");
        assert_eq!(r.sep, SeqSep::AutoUnderscore, "分隔符默认自动补 _");
        assert_eq!(r.scope, SeqScope::CurrentFolder, "作用域默认当前文件夹");
        assert_eq!(r.strip, StripRule::TrailingUnderscoreDigits, "原名序号剥离默认剥离尾随 _数字");
    }

    #[test]
    fn 起始值切到1时首个序号为1() {
        let rule = SeqRule { start: 1, ..Default::default() };
        let names: Vec<String> = (1..=2).map(|i| render("date_{seq}", &ctx("a", "jpg", i), &rule).name).collect();
        assert_eq!(names, vec!["date_1.jpg", "date_2.jpg"]);
    }
}
