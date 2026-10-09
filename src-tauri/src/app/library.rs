//! `LibraryUseCase`（执行版 §13.4：库定位 / 迁移 / 备份恢复 / **重新定位素材树**）。
//!
//! 索引以 `(volume_id, rel_path)` 定位（rel_path 相对卷根）。素材树整体改名、换盘、复制到别的
//! 机器之后，索引就全部对不上了。§13.4 要求的对策是「**重新定位素材树**」向导：
//!
//! 1. **高置信**：新根之下「相对结构 + 体积」都对得上 → 可批量改写，无需逐条确认；
//! 2. **待确认**：相对结构对不上，但「文件名 + 体积 + 创建/修改时间」唯一命中 → 交用户确认；
//! 3. **无法匹配**：只能保持 `missing`，**绝不静默删除任何元数据**。
//!
//! 分组、标签、保护区、已整理标记都挂在 `assets.id` 上，所以改写定位键不会丢任何用户数据。

use std::collections::HashMap;
use std::path::Path;
use std::sync::atomic::AtomicBool;

use rusqlite::Connection;

use crate::infra::db::{self, RelocateRow};
use crate::infra::library::LibraryLayout;
use crate::infra::{volume, walker};

/// 匹配置信度（§13.4 的三档）。
#[derive(Debug, Clone, Copy, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub enum Confidence {
    High,
    NeedsConfirm,
    Unmatched,
}

/// 一条条目的匹配结论。
#[derive(Debug, Clone, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct RelocateMatch {
    pub asset_id: i64,
    pub confidence: Confidence,
    /// 判定依据（要能展示给用户看，不能是黑箱）。
    pub why: String,
    pub old_rel_path: String,
    /// 改写后的相对卷根路径（无法匹配时为 `None`）。
    pub new_rel_path: Option<String>,
}

/// 整棵树的重定位计划。
#[derive(Debug, Clone, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct RelocatePlan {
    pub old_root: String,
    pub new_root: String,
    pub old_volume: String,
    pub new_volume: String,
    pub matches: Vec<RelocateMatch>,
    pub high: u64,
    pub needs_confirm: u64,
    pub unmatched: u64,
}

fn strip_prefix_ci(text: &str, prefix: &str) -> Option<String> {
    if prefix.is_empty() {
        return Some(text.to_string());
    }
    let t = text.to_lowercase();
    let p = prefix.to_lowercase();
    if t == p {
        return Some(String::new());
    }
    let with_sep = format!("{p}\\");
    if t.starts_with(&with_sep) {
        Some(text[with_sep.len()..].to_string())
    } else {
        None
    }
}

fn join_rel(prefix: &str, suffix: &str) -> String {
    if prefix.is_empty() {
        suffix.to_string()
    } else if suffix.is_empty() {
        prefix.to_string()
    } else {
        format!("{prefix}\\{suffix}")
    }
}

/// 只扫一遍新根（只读），建立三张查找表。
struct NewTree {
    volume: String,
    prefix: String,
    by_rel: HashMap<String, i64>,
    by_name_size_time: HashMap<(String, i64, i64), Vec<String>>,
    by_name_size: HashMap<(String, i64), Vec<String>>,
}

impl NewTree {
    fn scan(new_root: &Path) -> Self {
        let cancel = AtomicBool::new(false);
        let volume = volume::volume_id(new_root);
        let prefix = volume::rel_path_from_volume(new_root);
        let mut tree = NewTree {
            volume,
            prefix,
            by_rel: HashMap::new(),
            by_name_size_time: HashMap::new(),
            by_name_size: HashMap::new(),
        };
        let _ = walker::walk(new_root, &Default::default(), &cancel, |item| {
            if let walker::WalkItem::File(f) = item {
                let rel = f.rel_path.replace('/', "\\");
                let key = join_rel(&tree.prefix, &rel).to_lowercase();
                tree.by_rel.insert(key, f.size as i64);
                tree.by_name_size_time
                    .entry((f.name.to_lowercase(), f.size as i64, f.mtime_ms))
                    .or_default()
                    .push(rel.clone());
                tree.by_name_size
                    .entry((f.name.to_lowercase(), f.size as i64))
                    .or_default()
                    .push(rel);
            }
        });
        tree
    }

    fn full_rel(&self, rel_under_new: &str) -> String {
        join_rel(&self.prefix, rel_under_new)
    }
}

/// 生成重定位计划（不写任何东西）。
pub fn plan_relocate(old_root: &Path, new_root: &Path, rows: &[RelocateRow]) -> RelocatePlan {
    let old_volume = volume::volume_id(old_root);
    let old_prefix = volume::rel_path_from_volume(old_root);
    let tree = NewTree::scan(new_root);

    let mut plan = RelocatePlan {
        old_root: old_root.to_string_lossy().to_string(),
        new_root: new_root.to_string_lossy().to_string(),
        old_volume,
        new_volume: tree.volume.clone(),
        matches: Vec::new(),
        high: 0,
        needs_confirm: 0,
        unmatched: 0,
    };

    for row in rows {
        let mut push = |confidence: Confidence, why: String, new_rel: Option<String>| {
            match confidence {
                Confidence::High => plan.high += 1,
                Confidence::NeedsConfirm => plan.needs_confirm += 1,
                Confidence::Unmatched => plan.unmatched += 1,
            }
            plan.matches.push(RelocateMatch {
                asset_id: row.id,
                confidence,
                why,
                old_rel_path: row.rel_path.clone(),
                new_rel_path: new_rel,
            });
        };

        let Some(suffix) = strip_prefix_ci(&row.rel_path, &old_prefix) else {
            push(
                Confidence::Unmatched,
                "这条索引不在原素材根之下，无法按相对结构匹配".to_string(),
                None,
            );
            continue;
        };

        let candidate = tree.full_rel(&suffix);
        if let Some(size) = tree.by_rel.get(&candidate.to_lowercase()) {
            if *size == row.size {
                push(
                    Confidence::High,
                    "新位置相对结构一致且体积相同".to_string(),
                    Some(candidate),
                );
                continue;
            }
            push(
                Confidence::NeedsConfirm,
                format!("新位置同名但体积不同（索引 {} B / 磁盘 {} B）", row.size, size),
                Some(candidate),
            );
            continue;
        }

        if let Some(list) = tree
            .by_name_size_time
            .get(&(row.name.to_lowercase(), row.size, row.mtime))
        {
            if list.len() == 1 {
                let rel = tree.full_rel(&list[0]);
                push(
                    Confidence::NeedsConfirm,
                    "按「文件名 + 体积 + 修改时间」唯一命中".to_string(),
                    Some(rel),
                );
            } else {
                push(
                    Confidence::NeedsConfirm,
                    format!("按「文件名 + 体积 + 修改时间」命中 {} 个，需人工指定", list.len()),
                    None,
                );
            }
            continue;
        }

        if let Some(list) = tree.by_name_size.get(&(row.name.to_lowercase(), row.size)) {
            push(
                Confidence::NeedsConfirm,
                format!("按「文件名 + 体积」命中 {} 个，需人工确认", list.len()),
                None,
            );
            continue;
        }

        push(
            Confidence::Unmatched,
            "新素材树里找不到对应文件（保持缺失状态，元数据不删）".to_string(),
            None,
        );
    }

    plan
}

/// 执行重定位改写：高置信一律改写，`待确认` 需显式放开。返回改写条数。
pub fn apply(
    conn: &Connection,
    plan: &RelocatePlan,
    include_needs_confirm: bool,
) -> Result<usize, String> {
    let mut n = 0usize;
    for m in &plan.matches {
        let take = m.confidence == Confidence::High
            || (include_needs_confirm && m.confidence == Confidence::NeedsConfirm);
        if !take {
            continue;
        }
        let Some(new_rel) = &m.new_rel_path else { continue };
        db::rewrite_location(conn, m.asset_id, &plan.new_volume, new_rel)?;
        n += 1;
    }
    Ok(n)
}

/// 只保留**原素材根之下**的索引行：重定位向导不该把不相干的素材报成「无法匹配」。
pub fn rows_under(old_root: &Path, rows: &[RelocateRow]) -> Vec<RelocateRow> {
    let prefix = volume::rel_path_from_volume(old_root);
    rows.iter()
        .filter(|r| strip_prefix_ci(&r.rel_path, &prefix).is_some())
        .cloned()
        .collect()
}

/// 一站式：打开库 → 取旧卷全部索引 → 出计划 →（可选）应用。
pub fn run(
    layout: &LibraryLayout,
    old_root: &Path,
    new_root: &Path,
    apply_now: bool,
    include_needs_confirm: bool,
) -> Result<(RelocatePlan, usize), String> {
    let conn = db::open_library(layout)?;
    let old_volume = volume::volume_id(old_root);
    let all = db::assets_of_volume(&conn, &old_volume)?;
    let rows = rows_under(old_root, &all);
    let plan = plan_relocate(old_root, new_root, &rows);
    let changed = if apply_now {
        apply(&conn, &plan, include_needs_confirm)?
    } else {
        0
    };
    Ok((plan, changed))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::path::PathBuf;

    struct Scene {
        dir: PathBuf,
    }

    fn scene(name: &str) -> Scene {
        let dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("target")
            .join("luling-tests")
            .join("relocate")
            .join(name);
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        Scene { dir }
    }

    fn row(id: i64, rel_path: &str, name: &str, size: i64, mtime: i64) -> RelocateRow {
        RelocateRow {
            id,
            volume_id: "vol:OLD".to_string(),
            rel_path: rel_path.to_string(),
            name: name.to_string(),
            size,
            mtime,
        }
    }

    /// 造一条「旧根之下的相对卷根路径」。
    fn old_rel(old: &Path, under: &str) -> String {
        join_rel(&volume::rel_path_from_volume(old), under)
    }

    #[test]
    fn 结构一致且体积相同判为高置信() {
        let s = scene("high");
        let old = s.dir.join("旧素材");
        let new = s.dir.join("新素材");
        fs::create_dir_all(new.join("子")).unwrap();
        fs::write(new.join("子").join("a.png"), b"12345678").unwrap();
        fs::create_dir_all(&old).unwrap();

        let rows = vec![row(1, &old_rel(&old, r"子\a.png"), "a.png", 8, 111)];
        let plan = plan_relocate(&old, &new, &rows);
        assert_eq!(plan.high, 1);
        assert_eq!(plan.matches[0].confidence, Confidence::High);
        assert!(plan.matches[0]
            .new_rel_path
            .as_deref()
            .unwrap()
            .to_lowercase()
            .ends_with(r"新素材\子\a.png"));
    }

    #[test]
    fn 体积不同判为待确认() {
        let s = scene("size-diff");
        let old = s.dir.join("旧");
        let new = s.dir.join("新");
        fs::create_dir_all(new.join("子")).unwrap();
        fs::write(new.join("子").join("a.png"), b"1234567890").unwrap();
        fs::create_dir_all(&old).unwrap();

        let rows = vec![row(1, &old_rel(&old, r"子\a.png"), "a.png", 8, 111)];
        let plan = plan_relocate(&old, &new, &rows);
        assert_eq!(plan.needs_confirm, 1);
        assert!(plan.matches[0].why.contains("体积不同"));
    }

    #[test]
    fn 结构变了但名字体积时间唯一命中判为待确认() {
        let s = scene("moved");
        let old = s.dir.join("旧");
        let new = s.dir.join("新");
        fs::create_dir_all(new.join("别的目录")).unwrap();
        let f = new.join("别的目录").join("b.png");
        fs::write(&f, b"abcdefgh").unwrap();
        let mtime = fs::metadata(&f).unwrap().modified().unwrap();
        let mtime_ms = mtime
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_millis() as i64;
        fs::create_dir_all(&old).unwrap();

        // 索引里它在「原位置」，实际已挪到「别的目录」
        let rows = vec![row(1, &old_rel(&old, r"原位置\b.png"), "b.png", 8, mtime_ms)];
        let plan = plan_relocate(&old, &new, &rows);
        assert_eq!(plan.needs_confirm, 1, "plan={plan:?}");
        assert!(plan.matches[0].why.contains("唯一命中"));
        assert!(plan.matches[0]
            .new_rel_path
            .as_deref()
            .unwrap()
            .to_lowercase()
            .ends_with(r"新\别的目录\b.png"));
    }

    #[test]
    fn 找不到就是无法匹配且不给新路径() {
        let s = scene("unmatched");
        let old = s.dir.join("旧");
        let new = s.dir.join("新");
        fs::create_dir_all(&new).unwrap();
        fs::write(new.join("x.png"), b"1111").unwrap();
        fs::create_dir_all(&old).unwrap();

        let rows = vec![row(1, &old_rel(&old, "不存在.png"), "不存在.png", 8, 111)];
        let plan = plan_relocate(&old, &new, &rows);
        assert_eq!(plan.unmatched, 1);
        assert!(plan.matches[0].new_rel_path.is_none());
    }

    #[test]
    fn 多条命中时要求人工指定() {
        let s = scene("ambiguous");
        let old = s.dir.join("旧");
        let new = s.dir.join("新");
        fs::create_dir_all(new.join("一")).unwrap();
        fs::create_dir_all(new.join("二")).unwrap();
        let a = new.join("一").join("dup.png");
        let b = new.join("二").join("dup.png");
        fs::write(&a, b"12345678").unwrap();
        fs::write(&b, b"12345678").unwrap();
        // 让两条的 mtime 完全一致
        let t = filetime(&a);
        let _ = set_mtime(&b, t);
        let mtime = filetime(&b);
        fs::create_dir_all(&old).unwrap();

        let rows = vec![row(1, &old_rel(&old, "旧位置\\dup.png"), "dup.png", 8, mtime)];
        let plan = plan_relocate(&old, &new, &rows);
        assert_eq!(plan.needs_confirm, 1);
        assert!(plan.matches[0].new_rel_path.is_none(), "多条命中不能替你选");
    }

    fn filetime(p: &Path) -> i64 {
        fs::metadata(p)
            .unwrap()
            .modified()
            .unwrap()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_millis() as i64
    }

    fn set_mtime(p: &Path, ms: i64) -> std::io::Result<()> {
        let t = std::time::UNIX_EPOCH + std::time::Duration::from_millis(ms as u64);
        let f = fs::File::options().write(true).open(p)?;
        f.set_modified(t)
    }

    #[test]
    fn 只保留原根之下的索引行() {
        let s = scene("rows-under");
        let old = s.dir.join("旧");
        let other = s.dir.join("别的根");
        fs::create_dir_all(&old).unwrap();
        let rows = vec![
            row(1, &old_rel(&old, r"子\a.png"), "a.png", 8, 1),
            row(2, &old_rel(&other, "b.png"), "b.png", 8, 1),
        ];
        let kept = rows_under(&old, &rows);
        assert_eq!(kept.len(), 1);
        assert_eq!(kept[0].id, 1);
        // 同名前缀但不是子目录（`旧素材` vs `旧素材备份`）也不能误收
        let sibling = s.dir.join("旧备份");
        let rows2 = vec![row(3, &old_rel(&sibling, "c.png"), "c.png", 8, 1)];
        assert!(rows_under(&old, &rows2).is_empty());
    }

    #[test]
    fn 应用时高置信直接改写_待确认需显式放开() {
        let s = scene("apply");
        let old = s.dir.join("旧");
        let new = s.dir.join("新");
        fs::create_dir_all(new.join("子")).unwrap();
        fs::write(new.join("子").join("a.png"), b"12345678").unwrap();
        fs::create_dir_all(&old).unwrap();

        let layout = LibraryLayout::for_root(s.dir.join("库"));
        let conn = db::open_library(&layout).unwrap();
        db::upsert_asset(
            &conn,
            &db::AssetRecord {
                volume_id: volume::volume_id(&old),
                rel_path: old_rel(&old, r"子\a.png"),
                name: "a.png".into(),
                kind: "image".into(),
                size: 8,
                mtime: 111,
                ..Default::default()
            },
        )
        .unwrap();

        // 不应用：只是计划
        let (_plan, changed) = run(&layout, &old, &new, false, false).unwrap();
        assert_eq!(changed, 0);
        // 应用：高置信被改写
        let (plan, changed) = run(&layout, &old, &new, true, false).unwrap();
        assert_eq!(plan.high, 1);
        assert_eq!(changed, 1);
        let detail = db::asset_detail(
            &conn,
            &plan.new_volume,
            plan.matches[0].new_rel_path.as_deref().unwrap(),
        )
        .unwrap();
        assert!(detail.is_some(), "改写后应按新定位键查到这条素材");
    }
}
