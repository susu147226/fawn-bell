//! 命名预设与命名计划（执行版 §7.3.2 / §7.3.3 与 §7.2 预检）。
//!
//! 两块内容：
//! ① **预设**：内置 13 个（存库、不可删）+ 自定义增删改排 + 导入导出；套用只改模板文本，
//!    走「草稿 → 预检 → 提交」管线，**不产生任何磁盘写**（§7.3.2）。
//! ② **计划**：按顺序分配序号（剔除行不占号、不跳号）、渲染模板、合法化文件名、
//!    检测冲突与环，并给出**两阶段改名**的步骤（执行属 P6，这里只产出计划）。

use std::collections::{HashMap, HashSet};
use std::path::Path;

use rusqlite::Connection;

use crate::domain::naming::{self, NameCtx, SeqRule, BUILTIN_PRESETS};
use crate::infra::db;

/* ── 预设 ─────────────────────────────────────────────────────────── */

#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PresetDto {
    pub id: i64,
    pub base_name: String,
    pub label: Option<String>,
    pub template: String,
    pub is_builtin: bool,
    pub sort_order: i64,
}

fn to_dto(r: db::PresetRow) -> PresetDto {
    PresetDto {
        id: r.id,
        base_name: r.base_name,
        label: r.label,
        template: r.template,
        is_builtin: r.is_builtin,
        sort_order: r.sort_order,
    }
}

/// 列出全部预设（首次调用会把 13 个内置预设种进库）。
pub fn list(conn: &Connection) -> Result<Vec<PresetDto>, String> {
    db::seed_builtin_presets(conn, &BUILTIN_PRESETS)?;
    Ok(db::load_presets(conn)?.into_iter().map(to_dto).collect())
}

pub fn save(
    conn: &Connection,
    id: Option<i64>,
    base_name: &str,
    label: Option<&str>,
    template: &str,
) -> Result<i64, String> {
    if base_name.trim().is_empty() || template.trim().is_empty() {
        return Err("预设的基名与模板都不能为空".to_string());
    }
    db::upsert_preset(conn, id, base_name.trim(), label, template.trim())
}

pub fn remove(conn: &Connection, id: i64) -> Result<(), String> {
    db::delete_preset(conn, id)
}

pub fn reorder(conn: &Connection, ids: &[i64]) -> Result<(), String> {
    db::reorder_presets(conn, ids)
}

/// 导出为 JSON（由界面交给用户选择落点；软件自己不写素材目录）。
pub fn export_json(conn: &Connection) -> Result<String, String> {
    let presets = list(conn)?;
    serde_json::to_string_pretty(&presets).map_err(|e| e.to_string())
}

/// 从 JSON 导入（自定义预设；同名基名覆盖模板）。
pub fn import_json(conn: &Connection, json: &str) -> Result<usize, String> {
    let incoming: Vec<PresetDto> = serde_json::from_str(json).map_err(|e| format!("不是合法的预设 JSON：{e}"))?;
    let mut n = 0usize;
    for p in incoming {
        let all = list(conn)?;
        let existing = all.iter().find(|x| x.base_name == p.base_name);
        // 内置预设随程序分发、不可删不可改；导入时跳过，避免出现重复的「date」这种条目
        if existing.map(|x| x.is_builtin).unwrap_or(false) {
            continue;
        }
        db::upsert_preset(
            conn,
            existing.map(|x| x.id),
            &p.base_name,
            p.label.as_deref(),
            &p.template,
        )?;
        n += 1;
    }
    Ok(n)
}

/// 套用预设（§7.3.2 固定契约）：模板变为 `<基名>_{seq}`，并**立刻**给出前三项预览。
pub fn apply(
    conn: &Connection,
    id: i64,
    ext: &str,
    start: u64,
    rule: &SeqRule,
) -> Result<(String, Vec<String>), String> {
    let preset = list(conn)?
        .into_iter()
        .find(|p| p.id == id)
        .ok_or_else(|| "预设不存在".to_string())?;
    if preset.template.contains("{seq}") {
        // 内置预设的模板已经是 `<基名>_{seq}`；自定义预设按原样使用
    }
    let rule = SeqRule {
        start,
        ..*rule
    };
    let mut preview = Vec::new();
    for i in 0..3u64 {
        let ctx = NameCtx {
            stem: &preset.base_name,
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
            seq: rule.start + i,
            counter: None,
        };
        preview.push(naming::render(&preset.template, &ctx, &rule).name);
    }
    Ok((preset.template, preview))
}

/* ── 命名计划 ─────────────────────────────────────────────────────── */

/// 计划的一条输入（来自索引 + 磁盘真值）。
#[derive(Debug, Clone)]
pub struct PlanSource {
    pub asset_id: i64,
    pub abs: String,
    pub stem: String,
    pub ext: String,
    pub capture_time: Option<i64>,
    pub mtime: i64,
    pub ctime: i64,
    pub camera: Option<String>,
    pub width: Option<i64>,
    pub height: Option<i64>,
    pub group: Option<String>,
    pub parent: Option<String>,
    pub kind: String,
    pub hash8: Option<String>,
    /// 被用户逐行剔除（§7.3.3：不参与编号且不跳号）。
    pub excluded: bool,
}

#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PlanItem {
    pub asset_id: i64,
    pub src: String,
    pub dst: String,
    /// 分配到的序号（剔除行为 `None`）。
    pub seq: Option<u64>,
    pub excluded: bool,
    pub notes: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct RenamePlan {
    pub items: Vec<PlanItem>,
    pub notes: Vec<String>,
}

fn replace_file_name(abs: &str, new_name: &str) -> String {
    let p = Path::new(abs);
    match p.parent() {
        Some(dir) => dir.join(new_name).to_string_lossy().to_string(),
        None => new_name.to_string(),
    }
}

/// 生成改名计划。
///
/// - 序号按**计划顺序**从 `rule.start` 起连续分配，剔除行不占号（§7.3.3）；
/// - 每条渲染后做合法化，冲突（磁盘已存在 / 计划内撞名）写进该条 `notes`；
/// - 只读：不碰磁盘、不写库。
pub fn build_plan(
    template: &str,
    rule: &SeqRule,
    sources: &[PlanSource],
    existing: &HashSet<String>,
) -> RenamePlan {
    let mut notes: Vec<String> = Vec::new();
    let mut items: Vec<PlanItem> = Vec::new();
    let mut seq = rule.start;
    let mut used: HashSet<String> = HashSet::new();
    let existing_lower: HashSet<String> =
        existing.iter().map(|p| p.replace('/', "\\").to_lowercase()).collect();

    for s in sources {
        if s.excluded {
            items.push(PlanItem {
                asset_id: s.asset_id,
                src: s.abs.clone(),
                dst: String::new(),
                seq: None,
                excluded: true,
                notes: vec!["已剔除，不参与编号".to_string()],
            });
            continue;
        }

        let stem = naming::strip_original_seq(&s.stem, rule.strip);
        let ctx = NameCtx {
            stem: &stem,
            ext: &s.ext,
            camera: s.camera.as_deref(),
            width: s.width,
            height: s.height,
            group: s.group.as_deref(),
            parent: s.parent.as_deref(),
            kind: &s.kind,
            hash8: s.hash8.as_deref(),
            capture_time: s.capture_time,
            mtime: s.mtime,
            ctime: s.ctime,
            seq,
            counter: None,
        };
        let rendered = naming::render(template, &ctx, rule);
        let mut item_notes = rendered.notes.clone();

        let (clean, san_notes) = naming::sanitize(&rendered.name);
        item_notes.extend(san_notes);
        let final_name = if clean.trim().is_empty() { rendered.name.clone() } else { clean };
        let dst = replace_file_name(&s.abs, &final_name);

        let key = dst.replace('/', "\\").to_lowercase();
        let src_key = s.abs.replace('/', "\\").to_lowercase();
        if key != src_key && existing_lower.contains(&key) {
            item_notes.push(format!("`{dst}` 在磁盘上已存在"));
        }
        if key != src_key && !used.insert(key) {
            item_notes.push(format!("`{dst}` 与计划里另一条目标相同"));
        }

        items.push(PlanItem {
            asset_id: s.asset_id,
            src: s.abs.clone(),
            dst,
            seq: Some(seq),
            excluded: false,
            notes: item_notes,
        });
        seq += 1;
    }

    if items.iter().any(|i| !i.notes.is_empty() && !i.excluded) {
        notes.push("计划里有条目需要处理（见各条说明）".to_string());
    }
    RenamePlan { items, notes }
}

/// 环状改名检测：顺着「目标 → 某条的源」走，回到起点就是环（返回涉及的下标）。
///
/// 两阶段改名（先临时名再目标名）能无条件解决环与大小写伪冲突，但**必须让用户看见**
/// 哪些条目构成了环（§16 第 3 条要求预览里说清楚）。
pub fn detect_cycles(plan: &RenamePlan) -> Vec<usize> {
    let mut by_src: HashMap<String, usize> = HashMap::new();
    for (i, item) in plan.items.iter().enumerate() {
        if item.excluded {
            continue;
        }
        by_src.insert(item.src.replace('/', "\\").to_lowercase(), i);
    }
    let mut in_cycle: Vec<usize> = Vec::new();
    for (start, item) in plan.items.iter().enumerate() {
        if item.excluded {
            continue;
        }
        let mut cursor = item.dst.replace('/', "\\").to_lowercase();
        let mut steps = 0usize;
        while let Some(&next) = by_src.get(&cursor) {
            if next == start {
                in_cycle.push(start);
                break;
            }
            steps += 1;
            if steps > plan.items.len() {
                break;
            }
            let Some(n) = plan.items.get(next) else { break };
            cursor = n.dst.replace('/', "\\").to_lowercase();
        }
    }
    in_cycle.sort_unstable();
    in_cycle.dedup();
    in_cycle
}

/// 两阶段改名的两步（§12.3 关键实现要点）：①源 → 临时名 ②临时名 → 目标。
///
/// 临时名带前缀且不落素材树之外；执行与 journal 属 P6。
pub const TEMP_PREFIX: &str = ".luling-tmp-";

pub fn two_phase(plan: &RenamePlan) -> (Vec<(String, String)>, Vec<(String, String)>) {
    let mut step1 = Vec::new();
    let mut step2 = Vec::new();
    for (n, item) in plan.items.iter().enumerate() {
        if item.excluded || item.dst.is_empty() {
            continue;
        }
        // 仅大小写变化也必须走临时名（Windows 大小写不敏感，直接改会撞名），所以这里按**精确**路径比较
        if item.src.replace('/', "\\") == item.dst.replace('/', "\\") {
            continue;
        }
        let file_name = Path::new(&item.src)
            .file_name()
            .map(|s| s.to_string_lossy().to_string())
            .unwrap_or_else(|| "item".to_string());
        let dir = Path::new(&item.src)
            .parent()
            .map(|p| p.to_path_buf())
            .unwrap_or_default();
        let tmp = dir
            .join(format!("{TEMP_PREFIX}{n}-{file_name}"))
            .to_string_lossy()
            .to_string();
        step1.push((item.src.clone(), tmp.clone()));
        step2.push((tmp, item.dst.clone()));
    }
    (step1, step2)
}

/* ── 引用感知 ─────────────────────────────────────────────────────── */

/// 一条「新名字被引用命中」的记录。
#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct RefHit {
    pub item_index: usize,
    pub ref_count: i64,
    pub ref_name: String,
}

fn file_name_of(path: &str) -> String {
    Path::new(path)
        .file_name()
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_default()
}

/// 用给定的计数函数给计划补引用说明（便于单测；生产路径用 [`annotate_references`]）。
///
/// 条文口径（§6.3.1 / §7.3.3）：**默认不改写引用**，只在计划里把「新名字被 N 处引用命中」
/// 说清楚；是否同步改写由用户在危险确认里显式开启（执行属 P6）。
pub fn annotate_with<F: Fn(&str) -> i64>(plan: &mut RenamePlan, count: F) -> Vec<RefHit> {
    let mut hits = Vec::new();
    for (i, item) in plan.items.iter_mut().enumerate() {
        if item.excluded || item.dst.is_empty() {
            continue;
        }
        let src_name = file_name_of(&item.src);
        let dst_name = file_name_of(&item.dst);
        if dst_name.is_empty() || dst_name.eq_ignore_ascii_case(&src_name) {
            continue;
        }
        let n = count(&dst_name);
        if n > 0 {
            item.notes
                .push(format!("新名字被 {n} 处引用命中（默认不改写引用，提交前请确认）"));
            hits.push(RefHit {
                item_index: i,
                ref_count: n,
                ref_name: dst_name,
            });
        }
    }
    hits
}

/// 从库里的引用映射（`refs` 表）给计划补引用说明。
pub fn annotate_references(conn: &Connection, plan: &mut RenamePlan) -> Result<Vec<RefHit>, String> {
    let err: std::cell::RefCell<Option<String>> = std::cell::RefCell::new(None);
    let hits = annotate_with(plan, |name| match db::ref_count(conn, name) {
        Ok(n) => n,
        Err(e) => {
            *err.borrow_mut() = Some(e);
            0
        }
    });
    match err.into_inner() {
        Some(e) => Err(e),
        None => Ok(hits),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::domain::naming::{PadMode, SeqSep, StripRule};
    use crate::infra::library::LibraryLayout;
    use std::path::PathBuf;

    fn conn(name: &str) -> Connection {
        let dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("target")
            .join("luling-tests")
            .join("naming")
            .join(name);
        let _ = std::fs::remove_dir_all(&dir);
        db::open_library(&LibraryLayout::for_root(dir.join("库"))).unwrap()
    }

    fn src(id: i64, abs: &str, stem: &str) -> PlanSource {
        PlanSource {
            asset_id: id,
            abs: abs.to_string(),
            stem: stem.to_string(),
            ext: "jpg".to_string(),
            capture_time: None,
            mtime: 1_700_000_000_000,
            ctime: 0,
            camera: None,
            width: None,
            height: None,
            group: None,
            parent: None,
            kind: "image".to_string(),
            hash8: None,
            excluded: false,
        }
    }

    #[test]
    fn 内置预设种进库且不可删() {
        let c = conn("presets");
        let list1 = list(&c).unwrap();
        assert_eq!(list1.len(), 13);
        assert!(list1.iter().all(|p| p.is_builtin));
        assert_eq!(list1[0].base_name, "time");
        assert_eq!(list1[1].template, "date_{seq}");
        // 幂等：再列一次不会重复插入
        assert_eq!(list(&c).unwrap().len(), 13);
        let err = remove(&c, list1[0].id).unwrap_err();
        assert!(err.contains("不可删除"), "{err}");
    }

    #[test]
    fn 自定义预设可增改删排且零磁盘写() {
        let c = conn("custom");
        let id = save(&c, None, "wd_dot", Some("我的"), "wd_dot_{seq}").unwrap();
        let list1 = list(&c).unwrap();
        assert_eq!(list1.len(), 14);
        let mine = list1.iter().find(|p| p.id == id).unwrap();
        assert_eq!(mine.template, "wd_dot_{seq}");
        assert!(!mine.is_builtin);

        save(&c, Some(id), "wd_dot", Some("我的"), "wd_dot_v2_{seq}").unwrap();
        let list2 = list(&c).unwrap();
        assert_eq!(
            list2.iter().find(|p| p.id == id).unwrap().template,
            "wd_dot_v2_{seq}"
        );

        remove(&c, id).unwrap();
        assert_eq!(list(&c).unwrap().len(), 13);
    }

    #[test]
    fn 预设导入导出往返() {
        let c = conn("io");
        list(&c).unwrap();
        save(&c, None, "xwd_du", None, "xwd_du_{seq}").unwrap();
        let json = export_json(&c).unwrap();
        assert!(json.contains("xwd_du_{seq}"));

        let c2 = conn("io2");
        let n = import_json(&c2, &json).unwrap();
        assert_eq!(n, 1, "内置 13 个跳过，只导入自定义的那 1 条");
        let names: Vec<String> = list(&c2).unwrap().into_iter().map(|p| p.base_name).collect();
        assert!(names.contains(&"xwd_du".to_string()));
        // 内置 13 个不会被重复插入
        assert_eq!(names.iter().filter(|n| *n == "date").count(), 1);
    }

    #[test]
    fn 套用date预设立刻出前三项() {
        let c = conn("apply");
        list(&c).unwrap();
        let date = list(&c).unwrap().into_iter().find(|p| p.base_name == "date").unwrap();
        let rule = SeqRule::default();
        let (tpl, preview) = apply(&c, date.id, "png", 0, &rule).unwrap();
        assert_eq!(tpl, "date_{seq}");
        assert_eq!(preview, vec!["date_0.png", "date_1.png", "date_2.png"]);
    }

    #[test]
    fn 计划按顺序编号且剔除行不占号() {
        let rule = SeqRule::default();
        let sources = vec![
            src(1, r"D:\素材\IMG_0001.jpg", "IMG_0001"),
            src(2, r"D:\素材\IMG_0002.jpg", "IMG_0002"),
            src(3, r"D:\素材\IMG_0003.jpg", "IMG_0003"),
        ];
        let mut s = sources.clone();
        s[1].excluded = true;
        let plan = build_plan("date_{seq}", &rule, &s, &HashSet::new());
        assert_eq!(plan.items[0].seq, Some(0));
        assert!(plan.items[1].excluded);
        assert_eq!(plan.items[1].dst, "");
        assert_eq!(plan.items[2].seq, Some(1), "剔除行不占号：第三条拿 1 而不是 2");
        assert_eq!(plan.items[0].dst, r"D:\素材\date_0.jpg");
        assert_eq!(plan.items[2].dst, r"D:\素材\date_1.jpg");
    }

    #[test]
    fn 计划里的冲突会被标出() {
        let rule = SeqRule::default();
        let sources = vec![
            src(1, r"D:\素材\a.jpg", "a"),
            src(2, r"D:\素材\b.jpg", "b"),
        ];
        // 让第一条的目标落在磁盘已有文件上
        let mut mine = rule;
        mine.pad = PadMode::NoPad;
        let plan = build_plan("same", &mine, &sources, &HashSet::new());
        assert_eq!(plan.items[0].dst, r"D:\素材\same.jpg");
        assert_eq!(plan.items[1].dst, r"D:\素材\same.jpg");
        assert!(
            plan.items[1].notes.iter().any(|n| n.contains("另一条目标相同")),
            "第二条才该报「与另一条目标相同」：{:?}",
            plan.items[1].notes
        );

        let mut existing = HashSet::new();
        existing.insert(r"d:\素材\same.jpg".to_string());
        let plan2 = build_plan("same", &mine, &sources, &existing);
        assert!(plan2.items[0].notes.iter().any(|n| n.contains("已存在")));
    }

    #[test]
    fn 环状改名可被检出且两阶段能解开() {
        let rule = SeqRule::default();
        let mut a = src(1, r"D:\素材\a.jpg", "a");
        a.excluded = false;
        let mut b = src(2, r"D:\素材\b.jpg", "b");
        // 手工构造一个环：a→b、b→a
        let plan = RenamePlan {
            items: vec![
                PlanItem { asset_id: 1, src: a.abs.clone(), dst: b.abs.clone(), seq: Some(0), excluded: false, notes: vec![] },
                PlanItem { asset_id: 2, src: b.abs.clone(), dst: a.abs.clone(), seq: Some(1), excluded: false, notes: vec![] },
            ],
            notes: vec![],
        };
        let cycles = detect_cycles(&plan);
        assert_eq!(cycles, vec![0, 1], "两条都在环里");
        let (step1, step2) = two_phase(&plan);
        assert_eq!(step1.len(), 2);
        assert_eq!(step2.len(), 2);
        assert!(step1[0].1.contains(TEMP_PREFIX), "第一阶段先改临时名");
        assert_eq!(step1[0].1, step2[0].0, "第二阶段从临时名改到目标名");
        let _ = rule;
    }

    #[test]
    fn 大小写伪冲突也走两阶段() {
        let plan = RenamePlan {
            items: vec![PlanItem {
                asset_id: 1,
                src: r"D:\素材\a.JPG".to_string(),
                dst: r"D:\素材\a.jpg".to_string(),
                seq: Some(0),
                excluded: false,
                notes: vec![],
            }],
            notes: vec![],
        };
        let (step1, step2) = two_phase(&plan);
        assert_eq!(step1.len(), 1, "仅大小写变化也要先走临时名");
        assert_eq!(step2[0].1, r"D:\素材\a.jpg");
        // 仅大小写变化在 Windows 眼里是「同一个路径」，因此也算伪环——正是两阶段要解决的场景
        assert_eq!(detect_cycles(&plan), vec![0], "大小写伪冲突应被识别出来");
    }

    #[test]
    fn 引用命中会在计划里说明且默认不改写() {
        let mut plan = RenamePlan {
            items: vec![
                PlanItem {
                    asset_id: 1,
                    src: r"D:\素材\海边日落.jpg".to_string(),
                    dst: r"D:\素材\date_0.jpg".to_string(),
                    seq: Some(0),
                    excluded: false,
                    notes: vec![],
                },
                PlanItem {
                    asset_id: 2,
                    src: r"D:\素材\另一张.jpg".to_string(),
                    dst: r"D:\素材\date_1.jpg".to_string(),
                    seq: Some(1),
                    excluded: false,
                    notes: vec![],
                },
            ],
            notes: vec![],
        };
        // 假装 manifest 里引用了 date_0.jpg 两次
        let hits = annotate_with(&mut plan, |name| if name == "date_0.jpg" { 2 } else { 0 });
        assert_eq!(hits.len(), 1);
        assert_eq!(hits[0].item_index, 0);
        assert_eq!(hits[0].ref_count, 2);
        assert!(plan.items[0]
            .notes
            .iter()
            .any(|n| n.contains("被 2 处引用命中") && n.contains("默认不改写引用")));
        assert!(plan.items[1].notes.is_empty(), "没被引用的条目不该被加说明");
    }

    #[test]
    fn 起始值与补零档位在计划里生效() {
        let sources = vec![src(1, r"D:\素材\a.jpg", "a"), src(2, r"D:\素材\b.jpg", "b")];
        let rule = SeqRule {
            start: 1,
            pad: PadMode::Min2,
            sep: SeqSep::AutoUnderscore,
            strip: StripRule::TrailingUnderscoreDigits,
            ..Default::default()
        };
        let plan = build_plan("date_{seq}", &rule, &sources, &HashSet::new());
        assert_eq!(plan.items[0].dst, r"D:\素材\date_01.jpg");
        assert_eq!(plan.items[1].dst, r"D:\素材\date_02.jpg");
    }
}
