//! `DedupeUseCase`（执行版 §7.12 / §16⑰）。
//!
//! 两级指纹的**第二级**在这里落地：
//! 1. 索引里按「体积 + 首尾各 64 KB 分段哈希」取候选（[`crate::domain::dedupe::group_by_fingerprint`]）；
//! 2. 候选组内再做**逐字节全长比对**（[`crate::infra::hash::same_content`]），只有内容完全相同才算重复；
//! 3. 结果写进内置智能集合「重复内容」（`groups.kind = 'smart'` + `asset_group`），
//!    与 §7.12 要求的「判定与集合同源」一致——集合成员就是这一步的判定结果。
//!
//! 本用例**只读素材**：清理动作（只进回收站）属于 P7 文件操作矩阵，届时复用这里的重复组。

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};

use rusqlite::Connection;

use crate::domain::dedupe::{self, Candidate, KeepPolicy};
use crate::infra::db;
use crate::infra::hash;
use crate::infra::library::LibraryLayout;
use crate::infra::volume;

/// 内置「重复内容」智能集合名（§7.12）。
pub const COLLECTION_NAME: &str = "重复内容";
/// 集合规则（可重建，不是真源）。
pub const COLLECTION_RULE: &str = r#"{"type":"duplicateContent"}"#;

/// 重复组里的一个成员。
#[derive(Debug, Clone, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DuplicateMember {
    pub asset_id: i64,
    pub rel_path: String,
    /// 绝对路径（用于人工核对；取不到时为 `None`，不猜）。
    pub abs_path: Option<String>,
    pub size: i64,
    pub ctime: i64,
    /// 本组保留项（按当前保留策略）。
    pub keeper: bool,
}

/// 一个重复组（成员内容逐字节相同）。
#[derive(Debug, Clone, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DuplicateGroup {
    pub size: i64,
    pub hash_partial: String,
    pub members: Vec<DuplicateMember>,
    /// 除保留项外重复占用的字节数。
    pub waste_bytes: i64,
}

/// 去重结果（供 CLI 输出、界面「重复内容」面板与整理报告使用）。
#[derive(Debug, Clone, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DedupeReport {
    pub groups: Vec<DuplicateGroup>,
    /// 重复组数。
    pub group_count: u64,
    /// 重复项总数（全部成员）。
    pub duplicate_count: u64,
    /// 保留项数量（每组一个；手工策略下为 0）。
    pub keepers: u64,
    /// 可清理出来的字节数。
    pub waste_bytes: i64,
    /// 进入候选的条目数（第一级指纹筛出来的）。
    pub candidates: u64,
    pub policy: KeepPolicy,
    /// 「重复内容」集合的 id（写库失败时为 `None`）。
    pub collection_id: Option<i64>,
    pub warnings: Vec<String>,
    /// 因取消而提前结束（已判定完的组仍然有效）。
    pub cancelled: bool,
}

/// 用库目录打开库并跑一次去重。
pub fn run(
    layout: &LibraryLayout,
    policy: KeepPolicy,
    verify_content: bool,
    cancel: &AtomicBool,
) -> Result<DedupeReport, String> {
    let conn = db::open_library(layout)?;
    run_with(&conn, policy, verify_content, cancel)
}

/// 在已有连接上跑一次去重（便于单测与复用）。
pub fn run_with(
    conn: &Connection,
    policy: KeepPolicy,
    verify_content: bool,
    cancel: &AtomicBool,
) -> Result<DedupeReport, String> {
    let rows = db::duplicate_candidates(conn)?;
    let candidates: Vec<Candidate> = rows
        .iter()
        .map(|r| Candidate {
            asset_id: r.asset_id,
            volume_id: r.volume_id.clone(),
            rel_path: r.rel_path.clone(),
            size: r.size,
            hash_partial: r.hash_partial.clone(),
            ctime: r.ctime,
        })
        .collect();
    let roots = db::scan_roots_all(conn)?;

    let mut report = DedupeReport {
        groups: Vec::new(),
        group_count: 0,
        duplicate_count: 0,
        keepers: 0,
        waste_bytes: 0,
        candidates: candidates.len() as u64,
        policy,
        collection_id: None,
        warnings: Vec::new(),
        cancelled: false,
    };

    let mut members_for_collection: Vec<i64> = Vec::new();

    for group in dedupe::group_by_fingerprint(&candidates) {
        if cancel.load(Ordering::Relaxed) {
            report.cancelled = true;
            break;
        }

        // 第二级：候选组内按「全长内容」再分簇
        let mut clusters: Vec<Vec<usize>> = Vec::new();
        for &i in &group {
            let abs_i = abs_of(&roots, &candidates[i]);
            let mut placed = false;
            if verify_content {
                if let Some(ai) = abs_i.as_deref() {
                    for cluster in clusters.iter_mut() {
                        let Some(bj) = abs_of(&roots, &candidates[cluster[0]]) else {
                            continue;
                        };
                        match hash::same_content(ai, &bj) {
                            Ok(true) => {
                                cluster.push(i);
                                placed = true;
                                break;
                            }
                            Ok(false) => {}
                            Err(e) => {
                                report
                                    .warnings
                                    .push(format!("读取 {} 失败，无法完成内容比对：{e}", ai.display()));
                                break;
                            }
                        }
                    }
                } else {
                    // 定位不到绝对路径时必须说清楚，不能静默把它当成「不重复」（§12.4⑨ 不显示假结论）
                    report.warnings.push(format!(
                        "候选 `{}` 无法还原绝对路径（工作根未记录或卷不匹配），本次未做内容比对。",
                        candidates[i].rel_path
                    ));
                }
            }
            if !placed {
                clusters.push(vec![i]);
            }
        }

        for cluster in clusters {
            if cluster.len() < 2 {
                continue;
            }
            let src: Vec<Candidate> = cluster.iter().map(|&i| candidates[i].clone()).collect();
            let keeper = dedupe::pick_keeper(&src, policy);
            let waste = dedupe::waste_bytes(&src, keeper);
            let members: Vec<DuplicateMember> = cluster
                .iter()
                .enumerate()
                .map(|(pos, _)| DuplicateMember {
                    asset_id: src[pos].asset_id,
                    rel_path: src[pos].rel_path.clone(),
                    abs_path: abs_of(&roots, &src[pos]).map(|p| p.to_string_lossy().to_string()),
                    size: src[pos].size,
                    ctime: src[pos].ctime,
                    keeper: keeper == Some(pos),
                })
                .collect();
            report.duplicate_count += members.len() as u64;
            report.keepers += if keeper.is_some() { 1 } else { 0 };
            report.waste_bytes += waste;
            for m in &members {
                members_for_collection.push(m.asset_id);
            }
            report.groups.push(DuplicateGroup {
                size: src[0].size,
                hash_partial: src[0].hash_partial.clone(),
                members,
                waste_bytes: waste,
            });
        }
    }
    report.group_count = report.groups.len() as u64;

    // 「重复内容」智能集合：判定与集合同源（§7.12、§16⑰①）
    match db::ensure_smart_group(conn, COLLECTION_NAME, COLLECTION_RULE) {
        Ok(id) => {
            if let Err(e) = db::replace_group_members(conn, id, &members_for_collection) {
                report.warnings.push(format!("写入「重复内容」集合失败：{e}"));
            } else {
                report.collection_id = Some(id);
            }
        }
        Err(e) => report.warnings.push(format!("建立「重复内容」集合失败：{e}")),
    }

    Ok(report)
}

/// 把候选的 `(volume_id, rel_path)` 还原成绝对路径。
///
/// 索引存的是**相对卷根**的路径（§13.4），所以用扫描过的工作根反推：
/// 卷标识相同、且候选路径以该根在本卷内的相对路径为前缀时，拼回绝对路径。
fn abs_of(roots: &[String], c: &Candidate) -> Option<PathBuf> {
    for root in roots {
        let root_path = Path::new(root);
        if volume::volume_id(root_path) != c.volume_id {
            continue;
        }
        let root_rel = volume::rel_path_from_volume(root_path);
        // 用 push 而不是 format!：反斜杠在字面量里容易被转义成两个，这里保持唯一形态
        let mut prefix = root_rel.to_lowercase();
        prefix.push('\\');
        let rel_lower = c.rel_path.to_lowercase();
        if root_rel.is_empty() {
            return Some(root_path.join(&c.rel_path));
        }
        if rel_lower.starts_with(&prefix) {
            let rest = &c.rel_path[prefix.len()..];
            return Some(root_path.join(rest));
        }
        if rel_lower == root_rel.to_lowercase() {
            return Some(root_path.to_path_buf());
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::app::index::{self, NoMetadata};
    use crate::domain::kind::Kind;
    use crate::infra::library::LibraryLayout;
    use std::fs;

    fn scene(name: &str) -> (PathBuf, LibraryLayout) {
        let base = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("target")
            .join("luling-tests")
            .join("dedupe")
            .join(name);
        let _ = fs::remove_dir_all(&base);
        let root = base.join("素材");
        fs::create_dir_all(&root).unwrap();
        (root, LibraryLayout::for_root(base.join("库")))
    }

    fn index(root: &Path, layout: &LibraryLayout) {
        let cancel = AtomicBool::new(false);
        let mut files = Vec::new();
        crate::infra::walker::walk(root, &Default::default(), &cancel, |item| {
            if let crate::infra::walker::WalkItem::File(f) = item {
                files.push(f);
            }
        })
        .unwrap();
        index::index_scan(root, &files, layout, &cancel, &NoMetadata, |_| {}).unwrap();
    }

    #[test]
    fn 绝对路径还原可反推素材路径() {
        let root = r"D:\desktop\dsh workspace\.tmp-extract\dup".to_string();
        let c = Candidate {
            asset_id: 1,
            volume_id: "vol:583DB437".to_string(),
            rel_path: r"desktop\dsh workspace\.tmp-extract\dup\a.png".to_string(),
            size: 8,
            hash_partial: "x".to_string(),
            ctime: 0,
        };
        let got = abs_of(&[root.clone()], &c).expect("应能还原绝对路径");
        assert!(got.to_string_lossy().ends_with(r"dup\a.png"), "还原结果={got:?}");

        // 同名兄弟目录不能误匹配（前缀必须带分隔符）
        let sibling = r"D:\desktop\dsh workspace\.tmp-extract\dup2".to_string();
        let bad = Candidate {
            rel_path: r"desktop\dsh workspace\.tmp-extract\dup2\a.png".to_string(),
            volume_id: "vol:583DB437".to_string(),
            ..c.clone()
        };
        assert!(abs_of(&[sibling], &c).is_none());
        assert!(abs_of(&[root], &bad).is_none(), "不同根下的相对路径不得误配");
    }

    #[test]
    fn 同内容成组_同体积不同内容不成组() {
        let (root, layout) = scene("basic");
        fs::create_dir_all(root.join("子")).unwrap();
        let same = b"AAAAAAAABBBBBBBB";
        fs::write(root.join("a.png"), same).unwrap();
        fs::write(root.join("子").join("b.png"), same).unwrap();
        // 同体积（16 字节）但内容不同 → 必须不算重复（§16⑰①）
        fs::write(root.join("c.png"), b"AAAAAAAACCCCCCCC").unwrap();
        // 不同体积、内容也不同
        fs::write(root.join("d.png"), b"short").unwrap();
        index(&root, &layout);

        let conn = db::open_library(&layout).unwrap();
        let cancel = AtomicBool::new(false);
        let rep = run_with(&conn, KeepPolicy::EarliestCreated, true, &cancel).unwrap();
        assert_eq!(rep.group_count, 1, "只应有一组重复：{:?}", rep.groups);
        let g = &rep.groups[0];
        assert_eq!(g.members.len(), 2);
        assert_eq!(g.size, 16);
        assert!(g.hash_partial.starts_with("b3p1:"));
        assert_eq!(rep.duplicate_count, 2);
        assert_eq!(rep.keepers, 1);
        assert_eq!(rep.waste_bytes, 16);
        // 保留项唯一，且默认保留最早创建的（索引里存的是相对卷根的路径，§13.4）
        let keepers: Vec<&DuplicateMember> = g.members.iter().filter(|m| m.keeper).collect();
        assert_eq!(keepers.len(), 1);
        assert!(keepers[0].rel_path.ends_with("a.png"), "保留项={:?}", keepers[0].rel_path);
        assert!(keepers[0]
            .abs_path
            .as_deref()
            .unwrap_or_default()
            .ends_with(r"素材\a.png"));
        // 绝对路径可反推出来（用于人工核对）
        assert!(g.members.iter().all(|m| m.abs_path.is_some()));
    }

    #[test]
    fn 中段不同但首尾指纹相同的候选会被第二级否掉() {
        let (root, layout) = scene("full-compare");
        // 3×64KB：中段不同 → 首尾指纹相同，但全长内容不同
        let mut a = vec![7u8; 3 * 64 * 1024];
        let mut b = a.clone();
        a[64 * 1024 + 10] = 1;
        b[64 * 1024 + 20] = 2;
        fs::write(root.join("a.bin"), &a).unwrap();
        fs::write(root.join("b.bin"), &b).unwrap();
        // 一个真正相同的对照组
        fs::write(root.join("c.bin"), &a).unwrap();
        index(&root, &layout);

        let conn = db::open_library(&layout).unwrap();
        let cancel = AtomicBool::new(false);
        let rep = run_with(&conn, KeepPolicy::EarliestCreated, true, &cancel).unwrap();
        assert_eq!(rep.group_count, 1);
        let names: Vec<String> = rep.groups[0]
            .members
            .iter()
            .map(|m| {
                m.rel_path
                    .rsplit('\\')
                    .next()
                    .unwrap_or_default()
                    .to_string()
            })
            .collect();
        let mut sorted = names.clone();
        sorted.sort();
        assert_eq!(sorted, vec!["a.bin".to_string(), "c.bin".to_string()]);
        assert!(!names.contains(&"b.bin".to_string()), "中段不同的文件不得算重复");
    }

    #[test]
    fn 集合与判定同源且可手工策略() {
        let (root, layout) = scene("collection");
        fs::write(root.join("x.png"), b"1234567890").unwrap();
        fs::write(root.join("y.png"), b"1234567890").unwrap();
        index(&root, &layout);

        let conn = db::open_library(&layout).unwrap();
        let cancel = AtomicBool::new(false);
        let rep = run_with(&conn, KeepPolicy::Manual, true, &cancel).unwrap();
        assert_eq!(rep.group_count, 1);
        assert_eq!(rep.keepers, 0, "手工策略下核心域不替用户选保留项");
        let id = rep.collection_id.expect("应建立集合");
        let members = db::group_members(&conn, id).unwrap();
        assert_eq!(members.len(), 2, "集合成员必须与判定同源");
        // 再跑一次不会产生重复集合行
        let rep2 = run_with(&conn, KeepPolicy::EarliestCreated, true, &cancel).unwrap();
        assert_eq!(rep2.collection_id, Some(id));
        let count: i64 = conn
            .query_row("SELECT COUNT(*) FROM groups WHERE name = ?1", [COLLECTION_NAME], |r| r.get(0))
            .unwrap();
        assert_eq!(count, 1);
    }

    #[test]
    fn 无重复时集合为空() {
        let (root, layout) = scene("none");
        fs::write(root.join("only.png"), b"abc").unwrap();
        index(&root, &layout);
        let conn = db::open_library(&layout).unwrap();
        let cancel = AtomicBool::new(false);
        let rep = run_with(&conn, KeepPolicy::EarliestCreated, true, &cancel).unwrap();
        assert_eq!(rep.group_count, 0);
        assert_eq!(rep.duplicate_count, 0);
        let id = rep.collection_id.expect("集合仍应存在（空集合）");
        assert!(db::group_members(&conn, id).unwrap().is_empty());
    }

    #[test]
    fn 缺失条目不参与去重() {
        let (root, layout) = scene("missing-out");
        fs::write(root.join("a.png"), b"12345").unwrap();
        fs::write(root.join("b.png"), b"12345").unwrap();
        index(&root, &layout);
        fs::remove_file(root.join("b.png")).unwrap();
        index(&root, &layout); // 第二次扫描把 b.png 标为缺失

        let conn = db::open_library(&layout).unwrap();
        let cancel = AtomicBool::new(false);
        let rep = run_with(&conn, KeepPolicy::EarliestCreated, true, &cancel).unwrap();
        assert_eq!(rep.group_count, 0, "缺失条目不该再参与去重");
    }

    #[test]
    fn 种类字段不参与判定但缺失不影响() {
        let (root, layout) = scene("kinds");
        fs::create_dir_all(&root).unwrap();
        fs::write(root.join("a.mp4"), b"0123456789").unwrap();
        fs::write(root.join("b.mp4"), b"0123456789").unwrap();
        index(&root, &layout);
        let conn = db::open_library(&layout).unwrap();
        let files: Vec<(String, String)> = {
            let mut stmt = conn.prepare("SELECT kind, rel_path FROM assets ORDER BY rel_path").unwrap();
            let rows = stmt
                .query_map([], |r| Ok((r.get(0)?, r.get(1)?)))
                .unwrap()
                .map(|r| r.unwrap())
                .collect();
            rows
        };
        assert!(files.iter().all(|(k, _)| k == Kind::Video.as_str()));
        let cancel = AtomicBool::new(false);
        let rep = run_with(&conn, KeepPolicy::EarliestCreated, true, &cancel).unwrap();
        assert_eq!(rep.group_count, 1);
    }
}
