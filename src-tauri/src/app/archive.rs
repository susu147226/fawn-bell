//! `ArchiveUseCase`（执行版 §7.4 归档与位置整理）。
//!
//! 把核心域的判定拼成一份**可预览的归档批次**：目标目录 + 目标名 + 同盘/跨盘 + 冲突结论 +
//! 空目录清单 + 工程包联动项 + 被引用警告。**只读**：不搬任何文件（跨盘复制/校验/删源与 journal 属 P6）。
//!
//! 分层要求（§12.2）：本模块不做 IO——目标盘上「有没有同名、体积与时间是否一致」由调用方
//! （CLI / IPC）查好后以 `targets` 传进来，这里只做纯判定。

use std::collections::{HashMap, HashSet};

use crate::domain::archive::{self, ConflictPolicy, ConflictVerdict};
use crate::domain::naming::{self, NameCtx, SeqRule};

/// 一条归档输入（来自索引；`abs` 与磁盘一致）。
#[derive(Debug, Clone)]
pub struct ArchiveSource {
    pub asset_id: i64,
    /// 源所在卷标识（`vol:…`）。
    pub volume_id: String,
    pub abs: String,
    pub stem: String,
    pub ext: String,
    pub group: Option<String>,
    pub parent: Option<String>,
    pub kind: String,
    pub capture_time: Option<i64>,
    pub mtime: i64,
    pub ctime: i64,
    pub size: i64,
    pub excluded: bool,
}

/// 目标盘上已存在同名的信息：绝对路径（小写）→ (体积, 修改时间)。
pub type TargetFacts = HashMap<String, (i64, i64)>;

/// 归档请求（模板、目标根、策略与外部事实）。
pub struct ArchiveRequest<'a> {
    pub target_root: &'a str,
    /// 目标卷标识；与源卷不同即跨盘（§7.4 跨盘要复制 → 校验 → 删源）。
    pub target_volume: &'a str,
    pub dir_template: &'a str,
    pub name_template: &'a str,
    pub rule: &'a SeqRule,
    pub policy: ConflictPolicy,
    /// 空目录清理：默认**关闭**（§7.4）。
    pub clean_empty_dirs: bool,
    /// 目标盘上已存在的同名项（调用方查好）。
    pub targets: &'a TargetFacts,
    /// 源目录 → 该目录下的条目名（工程包联动与「归档后空目录」判定都要用）。
    pub siblings: &'a HashMap<String, Vec<String>>,
    /// 文件名（小写）→ 引用它的文件列表（§6.3.1）。
    pub referrers: &'a HashMap<String, Vec<String>>,
}

#[derive(Debug, Clone, PartialEq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ArchivePlanItem {
    pub asset_id: i64,
    pub src: String,
    pub dst_dir: String,
    /// 目标绝对路径（目录 + 文件名）
    pub dst: String,
    /// 同盘 = 元数据操作；跨盘 = 复制 → 校验 → 删源（§7.4）
    pub same_volume: bool,
    pub verdict: ConflictVerdict,
    /// 工程包联动项（默认一起移动，可在预览里逐项取消）
    pub companions: Vec<String>,
    pub notes: Vec<String>,
    pub excluded: bool,
}

#[derive(Debug, Clone, PartialEq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ArchivePlan {
    pub items: Vec<ArchivePlanItem>,
    pub target_root: String,
    pub dir_template: String,
    pub name_template: String,
    pub policy: ConflictPolicy,
    pub clean_empty_dirs: bool,
    /// 归档后会被清空的源目录（执行前必须列清单；是否真的清理由界面开关决定）
    pub empty_dirs: Vec<String>,
    pub notes: Vec<String>,
}

fn dir_of(abs: &str) -> String {
    match abs.rfind(['\\', '/']) {
        Some(i) => abs[..i].to_string(),
        None => String::new(),
    }
}

fn name_of(abs: &str) -> String {
    match abs.rfind(['\\', '/']) {
        Some(i) => abs[i + 1..].to_string(),
        None => abs.to_string(),
    }
}

fn join(root: &str, sub: &str) -> String {
    if sub.is_empty() {
        root.to_string()
    } else {
        format!("{}\\{sub}", root.trim_end_matches('\\'))
    }
}

/// 生成归档计划。序号（若模板用了 `{seq}`）按计划顺序从 `rule.start` 连续分配，剔除行不占号。
pub fn build_plan(req: &ArchiveRequest, sources: &[ArchiveSource]) -> ArchivePlan {
    let mut items: Vec<ArchivePlanItem> = Vec::new();
    let mut notes: Vec<String> = Vec::new();
    let mut seq = req.rule.start;
    let mut used: HashSet<String> = HashSet::new();
    // 源目录 → 本批次里从该目录移走的条目名
    let mut moved_from: HashMap<String, Vec<String>> = HashMap::new();

    for s in sources {
        if s.excluded {
            items.push(ArchivePlanItem {
                asset_id: s.asset_id,
                src: s.abs.clone(),
                dst_dir: String::new(),
                dst: String::new(),
                same_volume: archive::is_same_volume(&s.volume_id, req.target_volume),
                verdict: ConflictVerdict::Free,
                companions: Vec::new(),
                notes: vec!["已剔除，不参与本次归档".to_string()],
                excluded: true,
            });
            continue;
        }

        let ctx = NameCtx {
            stem: &s.stem,
            ext: &s.ext,
            camera: None,
            width: None,
            height: None,
            group: s.group.as_deref(),
            parent: s.parent.as_deref(),
            kind: &s.kind,
            hash8: None,
            capture_time: s.capture_time,
            mtime: s.mtime,
            ctime: s.ctime,
            seq,
            counter: None,
        };

        let (dir_rel, dir_notes) = archive::render_dir(req.dir_template, &ctx);
        let dst_dir = join(req.target_root, &dir_rel);
        let rendered = naming::render(req.name_template, &ctx, req.rule);
        let file_name = rendered.name.clone();
        let dst = format!("{dst_dir}\\{file_name}");
        let dst_key = dst.replace('/', "\\").to_lowercase();

        let mut item_notes: Vec<String> = dir_notes;
        item_notes.extend(rendered.notes);
        let src_key = s.abs.replace('/', "\\").to_lowercase();
        if dst_key == src_key {
            item_notes.push("目标与源相同，本次不会移动".to_string());
        }
        if !used.insert(dst_key.clone()) {
            item_notes.push(format!("`{file_name}` 与计划里另一条目标相同"));
        }

        // 冲突判定（§7.4）：目标不存在 → Free；存在且体积与时间都相同 → 视为重复跳过；否则冲突
        let (verdict, exists) = match req.targets.get(&dst_key) {
            Some((size, mtime)) => (
                archive::resolve_conflict(true, *size == s.size, *mtime == s.mtime),
                true,
            ),
            None => (archive::resolve_conflict(false, false, false), false),
        };
        match verdict {
            ConflictVerdict::DuplicateSkip => {
                item_notes.push("目标已存在且体积与修改时间一致：视为重复，**跳过并记录**".to_string())
            }
            ConflictVerdict::Conflict => {
                let tail = match req.policy {
                    ConflictPolicy::Suffix => format!("按策略加后缀：`{}`", archive::with_suffix(&file_name, 2)),
                    ConflictPolicy::Skip => "按策略跳过该项".to_string(),
                    ConflictPolicy::AbortBatch => "按策略中止整个批次".to_string(),
                };
                item_notes.push(format!("目标已存在且内容不同，绝不静默覆盖 → {tail}"));
            }
            ConflictVerdict::Free => {
                if exists {
                    // 目标与源同路径但存在性成立时不该走到这里；留个提示便于排查
                    item_notes.push("目标路径已存在".to_string());
                }
            }
        }

        // 被引用文件：只警告并列出引用方（§6.3.1）
        if let Some(w) = archive::reference_warning(&file_name, req.referrers.get(&file_name.to_lowercase()).map(|v| v.as_slice()).unwrap_or(&[])) {
            item_notes.push(w);
        }

        // 工程包联动（§7.4）：命中工程文件时，伴随文件与同名依赖目录默认一起移动
        let src_dir = dir_of(&s.abs);
        let empty: Vec<String> = Vec::new();
        let companions = if archive::is_project_file(&file_name) {
            archive::project_companions(&file_name, req.siblings.get(&src_dir).unwrap_or(&empty))
        } else {
            empty
        };
        if !companions.is_empty() {
            item_notes.push(format!("工程包联动：默认一并移动 {} 项", companions.len()));
        }

        moved_from.entry(src_dir).or_default().push(name_of(&s.abs));
        items.push(ArchivePlanItem {
            asset_id: s.asset_id,
            src: s.abs.clone(),
            dst_dir,
            dst,
            same_volume: archive::is_same_volume(&s.volume_id, req.target_volume),
            verdict,
            companions,
            notes: item_notes,
            excluded: false,
        });
        seq += 1;
    }

    // 归档后会被清空的源目录：该目录下所有条目都在本批次里
    let mut emptied: Vec<(String, usize)> = Vec::new();
    for (dir, moved) in &moved_from {
        let all = req.siblings.get(dir);
        let total = all.map(|v| v.len()).unwrap_or(moved.len());
        if moved.len() >= total {
            emptied.push((dir.clone(), 0usize));
        }
    }
    let empties_moved = archive::empty_dirs_after(&emptied);
    if !empties_moved.is_empty() {
        notes.push(format!(
            "归档后会有 {} 个源目录变空（空目录清理默认关闭：{}）",
            empties_moved.len(),
            if req.clean_empty_dirs { "本次开启" } else { "本次不开" }
        ));
    }
    let cross = items.iter().filter(|i| !i.excluded && !i.same_volume).count();
    if cross > 0 {
        notes.push(format!("{cross} 条是跨盘移动：提交时会走「复制 → 校验（体积 + 修改时间）→ 删除源 → 写 journal」"));
    }

    ArchivePlan {
        items,
        target_root: req.target_root.to_string(),
        dir_template: req.dir_template.to_string(),
        name_template: req.name_template.to_string(),
        policy: req.policy,
        clean_empty_dirs: req.clean_empty_dirs,
        empty_dirs: empties_moved,
        notes,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn src(id: i64, abs: &str, kind: &str) -> ArchiveSource {
        let name = name_of(abs);
        let (stem, ext) = match name.rsplit_once('.') {
            Some((s, e)) => (s.to_string(), e.to_string()),
            None => (name.clone(), String::new()),
        };
        ArchiveSource {
            asset_id: id,
            volume_id: "vol:AAA".into(),
            abs: abs.to_string(),
            stem,
            ext,
            group: None,
            parent: None,
            kind: kind.to_string(),
            capture_time: None,
            mtime: 1_700_000_000_000,
            ctime: 0,
            size: 100,
            excluded: false,
        }
    }

    fn empty_facts() -> (TargetFacts, HashMap<String, Vec<String>>, HashMap<String, Vec<String>>) {
        (HashMap::new(), HashMap::new(), HashMap::new())
    }

    #[test]
    fn 目录模板与命名模板一起算目标路径() {
        let (t, sib, refs) = empty_facts();
        let req = ArchiveRequest {
            target_root: r"D:\归档",
            target_volume: "vol:AAA",
            dir_template: "{kind}/{yyyy}/{mm}",
            name_template: "{name}_{seq}",
            rule: &SeqRule::default(),
            policy: ConflictPolicy::default(),
            clean_empty_dirs: false,
            targets: &t,
            siblings: &sib,
            referrers: &refs,
        };
        let plan = build_plan(&req, &[src(1, r"D:\素材\海边日落.jpg", "image")]);
        assert_eq!(plan.items[0].dst_dir, r"D:\归档\image\2023\11");
        assert_eq!(plan.items[0].dst, r"D:\归档\image\2023\11\海边日落_0.jpg");
        assert!(plan.items[0].same_volume);
        assert_eq!(plan.items[0].verdict, ConflictVerdict::Free);
    }

    #[test]
    fn 冲突三档在计划里如实呈现() {
        let mut t: TargetFacts = HashMap::new();
        t.insert(r"d:\归档\image\2023\11\海边日落_0.jpg".to_string(), (100, 1_700_000_000_000)); // 体积时间全同
        t.insert(r"d:\归档\image\2023\11\别的_1.jpg".to_string(), (999, 1)); // 第二条的序号是 1
        let sib = HashMap::new();
        let refs = HashMap::new();
        let req = ArchiveRequest {
            target_root: r"D:\归档",
            target_volume: "vol:AAA",
            dir_template: "{kind}/{yyyy}/{mm}",
            name_template: "{name}_{seq}",
            rule: &SeqRule::default(),
            policy: ConflictPolicy::Suffix,
            clean_empty_dirs: false,
            targets: &t,
            siblings: &sib,
            referrers: &refs,
        };
        let plan = build_plan(
            &req,
            &[src(1, r"D:\素材\海边日落.jpg", "image"), src(2, r"D:\素材\别的.jpg", "image")],
        );
        assert_eq!(plan.items[0].verdict, ConflictVerdict::DuplicateSkip);
        assert!(plan.items[0].notes.iter().any(|n| n.contains("视为重复")));
        assert_eq!(plan.items[1].verdict, ConflictVerdict::Conflict);
        assert!(plan.items[1].notes.iter().any(|n| n.contains("绝不静默覆盖") && n.contains("(2)")));
    }

    #[test]
    fn 跨盘会被标出并写进说明() {
        let (t, sib, refs) = empty_facts();
        let mut a = src(1, r"D:\素材\a.jpg", "image");
        a.volume_id = "vol:AAA".into();
        let req = ArchiveRequest {
            target_root: r"E:\归档",
            target_volume: "vol:BBB",
            dir_template: "{kind}",
            name_template: "{name}",
            rule: &SeqRule::default(),
            policy: ConflictPolicy::default(),
            clean_empty_dirs: false,
            targets: &t,
            siblings: &sib,
            referrers: &refs,
        };
        let plan = build_plan(&req, &[a]);
        assert!(!plan.items[0].same_volume);
        assert!(plan.notes.iter().any(|n| n.contains("跨盘移动") && n.contains("校验")));
    }

    #[test]
    fn 空目录清单与工程包联动() {
        let (t, _, refs) = empty_facts();
        let mut sib: HashMap<String, Vec<String>> = HashMap::new();
        sib.insert(
            r"D:\素材".to_string(),
            vec!["模型_03.blend".to_string(), "模型_03.blend1".to_string()],
        );
        let req = ArchiveRequest {
            target_root: r"D:\归档",
            target_volume: "vol:AAA",
            dir_template: "{kind}",
            name_template: "{name}",
            rule: &SeqRule::default(),
            policy: ConflictPolicy::default(),
            clean_empty_dirs: false,
            targets: &t,
            siblings: &sib,
            referrers: &refs,
        };
        let mut files = vec![
            src(1, r"D:\素材\模型_03.blend", "3d"),
            src(2, r"D:\素材\模型_03.blend1", "3d"),
        ];
        files[0].stem = "模型_03".into();
        files[0].ext = "blend".into();
        let plan = build_plan(&req, &files);
        assert_eq!(plan.items[0].companions, vec!["模型_03.blend1".to_string()]);
        assert!(plan.items[0].notes.iter().any(|n| n.contains("工程包联动")));
        assert_eq!(plan.empty_dirs, vec![r"D:\素材".to_string()], "整目录搬空 → 列进空目录清单");
        assert!(plan.notes.iter().any(|n| n.contains("空目录清理默认关闭")));
        assert!(!plan.clean_empty_dirs, "默认关闭");
    }

    #[test]
    fn 被引用文件只警告不改写() {
        let (t, sib, _) = empty_facts();
        let mut refs: HashMap<String, Vec<String>> = HashMap::new();
        refs.insert("海边日落.jpg".to_string(), vec!["manifest.xml".to_string()]);
        let req = ArchiveRequest {
            target_root: r"D:\归档",
            target_volume: "vol:AAA",
            dir_template: "{kind}",
            name_template: "{name}",
            rule: &SeqRule::default(),
            policy: ConflictPolicy::default(),
            clean_empty_dirs: false,
            targets: &t,
            siblings: &sib,
            referrers: &refs,
        };
        let plan = build_plan(&req, &[src(1, r"D:\素材\海边日落.jpg", "image")]);
        assert!(plan.items[0]
            .notes
            .iter()
            .any(|n| n.contains("被 1 处引用") && n.contains("默认不改写引用")));
    }

    #[test]
    fn 剔除行不占号也不参与归档() {
        let (t, sib, refs) = empty_facts();
        let req = ArchiveRequest {
            target_root: r"D:\归档",
            target_volume: "vol:AAA",
            dir_template: "{kind}",
            name_template: "{name}_{seq}",
            rule: &SeqRule::default(),
            policy: ConflictPolicy::default(),
            clean_empty_dirs: false,
            targets: &t,
            siblings: &sib,
            referrers: &refs,
        };
        let mut a = src(1, r"D:\素材\a.jpg", "image");
        let mut b = src(2, r"D:\素材\b.jpg", "image");
        let c = src(3, r"D:\素材\c.jpg", "image");
        b.excluded = true;
        a.stem = "a".into();
        let plan = build_plan(&req, &[a, b, c]);
        assert_eq!(plan.items[0].dst, r"D:\归档\image\a_0.jpg");
        assert!(plan.items[1].excluded);
        assert_eq!(plan.items[2].dst, r"D:\归档\image\c_1.jpg", "剔除行不占号");
    }
}
