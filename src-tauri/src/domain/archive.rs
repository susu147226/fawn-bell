//! 核心域 · 归档与位置整理（执行版 §7.4，**纯逻辑、无 IO**）。
//!
//! 条文里的六件事，能在这里算的都算在这里：
//! ① 目录模板（与命名模板**同占位符同修饰符**，但不补扩展名）；② 同盘 / 跨盘判定；
//! ③ 冲突判定（目标已存在同名时比对体积与时间：完全相同→视为重复跳过；不同→按策略处理，
//! **绝不静默覆盖**）；④ 空目录清理清单（默认关闭，先出清单）；⑤ 工程包联动（伴随文件与同名依赖目录）；
//! ⑥ 被引用文件只警告、列出引用方，默认不改写引用。
//!
//! 真正的搬运（复制 → 校验 → 删源 → 写 journal）属 P6 提交执行。

use crate::domain::naming::{self, NameCtx, SeqRule};

/// 渲染归档**目录**模板（§7.4）：`{kind}/{yyyy}/{mm}`、`{group}/`、`{date:yyyy-MM}`、`{camera}/{yyyy}`…
///
/// 与命名模板共用同一套占位符与修饰符（`naming::render_with`，`append_ext=false`），
/// 每一段再过一遍合法化——目录名同样不能带 `\ / : * ? " < > |` 或结尾点空格。
pub fn render_dir(template: &str, ctx: &NameCtx) -> (String, Vec<String>) {
    let rendered = naming::render_with(template, ctx, &SeqRule::default(), false);
    let mut notes: Vec<String> = rendered.notes.clone();
    let mut segs: Vec<String> = Vec::new();
    for raw in rendered.name.split(['/', '\\']) {
        if raw.trim().is_empty() {
            continue;
        }
        let (clean, mut n) = naming::sanitize(raw);
        notes.append(&mut n);
        segs.push(if clean.trim().is_empty() { "_".to_string() } else { clean });
    }
    if segs.is_empty() {
        notes.push("目录模板渲染为空，已回退为不分目录（直接放目标根）".to_string());
    }
    (segs.join("\\"), notes)
}

/// 同盘判定（§7.4：同盘移动是元数据操作，瞬时完成；跨盘要复制 → 校验 → 删源 → 写 journal）。
pub fn is_same_volume(src_volume: &str, dst_volume: &str) -> bool {
    src_volume.eq_ignore_ascii_case(dst_volume)
}

/// 目标已存在同名文件时的冲突策略（§7.4）。
#[derive(Debug, Clone, Copy, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum ConflictPolicy {
    /// 加后缀（`name (2).ext`）
    Suffix,
    /// 跳过该项并记录
    Skip,
    /// 中止整个批次
    AbortBatch,
}

impl Default for ConflictPolicy {
    fn default() -> Self {
        ConflictPolicy::Suffix
    }
}

/// 冲突判定结论。
#[derive(Debug, Clone, Copy, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub enum ConflictVerdict {
    /// 目标不存在，直接落位
    Free,
    /// 目标存在但体积与修改时间都相同 → 视为重复，**跳过并记录**
    DuplicateSkip,
    /// 目标存在且不同 → 按策略处理
    Conflict,
}

/// 判定单个目标的冲突（§7.4：完全相同 → 重复跳过；不同 → 冲突；绝不静默覆盖）。
pub fn resolve_conflict(target_exists: bool, same_size: bool, same_mtime: bool) -> ConflictVerdict {
    if !target_exists {
        ConflictVerdict::Free
    } else if same_size && same_mtime {
        ConflictVerdict::DuplicateSkip
    } else {
        ConflictVerdict::Conflict
    }
}

/// 加后缀消歧（从 2 开始）。
pub fn with_suffix(file_name: &str, n: u32) -> String {
    match file_name.rsplit_once('.') {
        Some((stem, ext)) if !stem.is_empty() && !ext.is_empty() => format!("{stem} ({n}).{ext}"),
        _ => format!("{file_name} ({n})"),
    }
}

/// 空目录清理清单（§7.4：默认**关闭**；执行前必须列出将被删除的目录清单）。
///
/// 输入是「归档后每个目录还剩多少条目」的快照；输出按**深度从深到浅**排序——
/// 子目录被删后父目录可能才变空，深先浅后与用户的直觉一致。
pub fn empty_dirs_after(after_counts: &[(String, usize)]) -> Vec<String> {
    let mut out: Vec<String> = after_counts
        .iter()
        .filter(|(_, n)| *n == 0)
        .map(|(p, _)| p.clone())
        .collect();
    out.sort_by(|a, b| {
        b.matches('\\')
            .count()
            .cmp(&a.matches('\\').count())
            .then_with(|| a.cmp(b))
    });
    out
}

/// 工程文件扩展名（§6.3 的判定之一）。
pub const PROJECT_EXTS: [&str; 6] = ["blend", "max", "ma", "mb", "c4d", "ztl"];

pub fn is_project_file(name: &str) -> bool {
    match name.rsplit_once('.') {
        Some((_, ext)) => PROJECT_EXTS.iter().any(|e| e.eq_ignore_ascii_case(ext)),
        None => false,
    }
}

/// 工程包联动（§7.4 / §6.3）：给定工程文件名与同目录条目，返回**默认一起移动**的伴随项
/// （同名依赖目录、`场景.blend1` / `场景.blend.bak` 这类伴生文件）。预览里可逐项取消。
pub fn project_companions(project_file: &str, siblings: &[String]) -> Vec<String> {
    let stem = project_file
        .rsplit_once('.')
        .map(|(s, _)| s)
        .unwrap_or(project_file);
    let stem_lower = stem.to_lowercase();
    let mut out = Vec::new();
    for s in siblings {
        if s.eq_ignore_ascii_case(project_file) {
            continue;
        }
        // 同名依赖目录（Blender 的 `场景/` 之类）
        if s.eq_ignore_ascii_case(stem) {
            out.push(s.clone());
            continue;
        }
        // 同名伴随文件：`场景.blend1`、`场景.blend2`、`场景.blend.bak`、`场景.bak`
        let s_lower = s.to_lowercase();
        if let Some(rest) = s_lower.strip_prefix(&stem_lower) {
            if rest.starts_with(".blend") || rest.starts_with(".bak") || rest.starts_with('1') {
                out.push(s.clone());
            }
        }
    }
    out
}

/// 被引用文件的移动 / 改名警告（§7.4 + §6.3.1）：默认**只警告并列出引用方**，不改写引用。
pub fn reference_warning(name: &str, referrers: &[String]) -> Option<String> {
    if referrers.is_empty() {
        None
    } else {
        Some(format!(
            "`{name}` 被 {} 处引用（{}）；默认不改写引用，提交前请确认",
            referrers.len(),
            referrers.join("、")
        ))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ctx<'a>(kind: &'a str, stem: &'a str, ext: &'a str) -> NameCtx<'a> {
        NameCtx {
            stem,
            ext,
            camera: None,
            width: None,
            height: None,
            group: None,
            parent: None,
            kind,
            hash8: None,
            capture_time: None,
            // 2023-11-14 22:13:20 UTC
            mtime: 1_700_000_000_000,
            ctime: 0,
            seq: 0,
            counter: None,
        }
    }

    #[test]
    fn 目录模板按kind年月分目录() {
        let c = ctx("image", "海边日落", "jpg");
        let (dir, notes) = render_dir("{kind}/{yyyy}/{mm}", &c);
        assert_eq!(dir, "image\\2023\\11");
        assert!(notes.is_empty(), "{notes:?}");
    }

    #[test]
    fn 目录模板支持date修饰符与分组() {
        let mut c = ctx("video", "a", "mp4");
        c.group = Some("旅行");
        let (dir, _) = render_dir("{group}/{date:yyyy-MM}", &c);
        assert_eq!(dir, "旅行\\2023-11");
    }

    #[test]
    fn 目录名同样过合法化() {
        let mut c = ctx("image", "a", "jpg");
        c.group = Some("客户:交付"); // 冒号是非法字符
        let (dir, notes) = render_dir("{group}", &c);
        assert_eq!(dir, "客户_交付");
        assert!(!notes.is_empty(), "应当留下「非法字符已替换」的说明");
    }

    #[test]
    fn 同盘判定忽略大小写() {
        assert!(is_same_volume("vol:583DB437", "vol:583db437"));
        assert!(!is_same_volume("vol:583DB437", "vol:11111111"));
    }

    #[test]
    fn 冲突判定三档() {
        assert_eq!(resolve_conflict(false, false, false), ConflictVerdict::Free);
        assert_eq!(
            resolve_conflict(true, true, true),
            ConflictVerdict::DuplicateSkip,
            "体积与时间都相同 → 视为重复跳过"
        );
        assert_eq!(resolve_conflict(true, true, false), ConflictVerdict::Conflict);
        assert_eq!(resolve_conflict(true, false, true), ConflictVerdict::Conflict);
    }

    #[test]
    fn 加后缀从不覆盖原名字() {
        assert_eq!(with_suffix("海边日落.jpg", 2), "海边日落 (2).jpg");
        assert_eq!(with_suffix("没有扩展名", 3), "没有扩展名 (3)");
        assert_eq!(with_suffix("a.tar.gz", 2), "a.tar (2).gz");
    }

    #[test]
    fn 空目录清单深的在前() {
        let rows = vec![
            ("素材\\2026".to_string(), 0usize),
            ("素材\\2026\\09".to_string(), 0),
            ("素材\\2026\\10".to_string(), 3),
        ];
        let out = empty_dirs_after(&rows);
        assert_eq!(out, vec!["素材\\2026\\09".to_string(), "素材\\2026".to_string()]);
    }

    #[test]
    fn 工程包联动挑出伴随文件与同名目录() {
        assert!(is_project_file("模型资产_03.blend"));
        assert!(!is_project_file("照片.jpg"));
        let siblings = vec![
            "模型资产_03.blend".to_string(),
            "模型资产_03".to_string(),        // 同名依赖目录
            "模型资产_03.blend1".to_string(), // Blender 备份
            "模型资产_03.blend.bak".to_string(),
            "别的文件.txt".to_string(),
        ];
        let got = project_companions("模型资产_03.blend", &siblings);
        assert!(got.contains(&"模型资产_03".to_string()));
        assert!(got.contains(&"模型资产_03.blend1".to_string()));
        assert!(got.contains(&"模型资产_03.blend.bak".to_string()));
        assert!(!got.contains(&"别的文件.txt".to_string()), "无关文件不该被卷进来");
    }

    #[test]
    fn 引用警告只警告不改写() {
        assert_eq!(reference_warning("a.png", &[]), None);
        let w = reference_warning("a.png", &["manifest.xml".to_string()]).unwrap();
        assert!(w.contains("被 1 处引用") && w.contains("manifest.xml") && w.contains("默认不改写引用"));
    }
}
