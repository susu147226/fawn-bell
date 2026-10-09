//! `GroupsUseCase` 与 `ProtectionUseCase`（执行版 §7.5 分组 / §7.6 保护区）。
//!
//! 两条硬口径：
//! ① **保护区不是分组**——它是横切于分组与目录结构之上的安全属性，独立存 `protections` 表；
//!    删分组、改分组都不会动它（§7.6）。
//! ② **智能集合是动态求值**——库里只存规则串，每次打开现算，新增素材自动落入（§7.5）。
//!
//! 本模块不写素材树：分组、保护区、已整理标记全部落库目录（§14①）。

use rusqlite::Connection;

use crate::infra::db::{self, GroupRow, ProtectionRow};

/// 内置智能集合（§7.5：随程序提供；内置集合可另存为自定义集合后修改）。
pub const BUILTIN_SMART: [(&str, &str); 4] = [
    ("重复内容", db::SMART_DUPLICATES),
    ("近 30 天新增", db::SMART_NEW_30D),
    ("未分类", db::SMART_UNTAGGED),
    ("大于 50 MB", db::SMART_BIG_50MB),
];

fn now_ms() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as i64)
        .unwrap_or(0)
}

/* ── 分组 ─────────────────────────────────────────────────────────── */

/// 幂等地把内置智能集合种进库。
pub fn ensure_builtin_smart(conn: &Connection) -> Result<usize, String> {
    let mut n = 0usize;
    for (name, rule) in BUILTIN_SMART {
        let exists: i64 = conn
            .query_row(
                "SELECT COUNT(*) FROM groups WHERE name = ?1 AND kind = 'smart'",
                rusqlite::params![name],
                |r| r.get(0),
            )
            .unwrap_or(0);
        if exists == 0 {
            db::create_group(conn, name, "smart", Some(rule))?;
            n += 1;
        }
    }
    Ok(n)
}

pub fn list(conn: &Connection) -> Result<Vec<GroupRow>, String> {
    ensure_builtin_smart(conn)?;
    db::groups_all(conn)
}

pub fn create(conn: &Connection, name: &str) -> Result<i64, String> {
    db::create_group(conn, name, "static", None)
}

pub fn rename(conn: &Connection, id: i64, name: &str) -> Result<(), String> {
    db::rename_group(conn, id, name)
}

/// 删除分组：**只删分组与成员关系**；保护区、标签、已整理标记一律不动（§7.6）。
pub fn remove(conn: &Connection, id: i64) -> Result<(), String> {
    db::delete_group(conn, id)
}

pub fn add_members(conn: &Connection, group_id: i64, asset_ids: &[i64]) -> Result<usize, String> {
    db::add_group_members(conn, group_id, asset_ids)
}

pub fn remove_members(conn: &Connection, group_id: i64, asset_ids: &[i64]) -> Result<usize, String> {
    db::remove_group_members(conn, group_id, asset_ids)
}

/// 求值智能集合规则。
///
/// `duplicates` 规则**不在这里重算**：内容去重的判定是「分段哈希 + 全长比对」，属 `app::dedupe`；
/// 调用方（IPC / CLI）把去重结果传进来，保证「重复内容」集合与去重判定**同源**（§16⑰①）。
pub fn eval_smart(conn: &Connection, rule_json: &str, duplicates: &[i64]) -> Result<Vec<i64>, String> {
    let v: serde_json::Value =
        serde_json::from_str(rule_json).map_err(|e| format!("智能集合规则不是合法 JSON：{e}"))?;
    let kind = v.get("kind").and_then(|k| k.as_str()).unwrap_or("");
    match kind {
        "duplicates" => Ok(duplicates.to_vec()),
        "newerThanDays" => {
            let days = v.get("days").and_then(|d| d.as_i64()).unwrap_or(30);
            let since = now_ms() - days * 24 * 60 * 60 * 1000;
            let mut stmt = conn
                .prepare("SELECT id FROM assets WHERE COALESCE(mtime, 0) >= ?1 ORDER BY id")
                .map_err(|e| e.to_string())?;
            let ids = stmt
                .query_map(rusqlite::params![since], |r| r.get::<_, i64>(0))
                .map_err(|e| e.to_string())?;
            let mut out = Vec::new();
            for id in ids {
                out.push(id.map_err(|e| e.to_string())?);
            }
            Ok(out)
        }
        "sizeGreaterThan" => {
            let bytes = v.get("bytes").and_then(|b| b.as_i64()).unwrap_or(0);
            let mut stmt = conn
                .prepare("SELECT id FROM assets WHERE COALESCE(size, 0) > ?1 ORDER BY id")
                .map_err(|e| e.to_string())?;
            let ids = stmt
                .query_map(rusqlite::params![bytes], |r| r.get::<_, i64>(0))
                .map_err(|e| e.to_string())?;
            let mut out = Vec::new();
            for id in ids {
                out.push(id.map_err(|e| e.to_string())?);
            }
            Ok(out)
        }
        // 未分类：不属于任何分组的素材（标签在 P4 后续小块里接）
        "untagged" => {
            let mut stmt = conn
                .prepare(
                    "SELECT a.id FROM assets a
                     WHERE NOT EXISTS (SELECT 1 FROM asset_group m WHERE m.asset_id = a.id)
                     ORDER BY a.id",
                )
                .map_err(|e| e.to_string())?;
            let ids = stmt.query_map([], |r| r.get::<_, i64>(0)).map_err(|e| e.to_string())?;
            let mut out = Vec::new();
            for id in ids {
                out.push(id.map_err(|e| e.to_string())?);
            }
            Ok(out)
        }
        other => Err(format!("不认识的智能集合规则 `{other}`")),
    }
}

/* ── 保护区 ───────────────────────────────────────────────────────── */

pub fn list_protected(conn: &Connection) -> Result<Vec<ProtectionRow>, String> {
    db::protections_all(conn)
}

pub fn protect(conn: &Connection, asset_ids: &[i64], by: &str, reason: Option<&str>) -> Result<usize, String> {
    db::protect_assets(conn, asset_ids, by, reason)
}

pub fn unprotect(conn: &Connection, asset_ids: &[i64]) -> Result<usize, String> {
    db::unprotect_assets(conn, asset_ids)
}

/// 「全部移出」（界面需二次确认，§7.6）。
pub fn unprotect_all(conn: &Connection) -> Result<usize, String> {
    db::unprotect_all(conn)
}

/// 统计口径（§7.6 / §16④）：总条目数 + **当周新增 N 项**。
pub fn stats(conn: &Connection) -> Result<(i64, i64), String> {
    Ok((db::protected_count(conn)?, db::protected_week_new(conn, now_ms())?))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::infra::db::AssetRecord;
    use crate::infra::library::LibraryLayout;
    use std::path::PathBuf;

    fn conn(name: &str) -> Connection {
        let dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("target")
            .join("luling-tests")
            .join("groups")
            .join(name);
        let _ = std::fs::remove_dir_all(&dir);
        db::open_library(&LibraryLayout::for_root(dir.join("库"))).unwrap()
    }

    fn asset(c: &Connection, rel: &str, size: i64, mtime: i64) -> i64 {
        db::upsert_asset(
            c,
            &AssetRecord {
                volume_id: "v".into(),
                rel_path: rel.into(),
                name: rel.rsplit('/').next().unwrap_or(rel).into(),
                kind: "image".into(),
                size,
                mtime,
                ..Default::default()
            },
        )
        .unwrap()
    }

    #[test]
    fn 内置智能集合幂等种入() {
        let c = conn("builtin");
        assert_eq!(ensure_builtin_smart(&c).unwrap(), 4);
        assert_eq!(ensure_builtin_smart(&c).unwrap(), 0, "第二次不再重复种");
        let all = list(&c).unwrap();
        assert_eq!(all.len(), 4);
        assert!(all.iter().all(|g| g.kind == "smart"));
        assert!(all.iter().any(|g| g.name == "重复内容"));
    }

    #[test]
    fn 智能集合动态求值() {
        let c = conn("smart");
        let old = asset(&c, "旧.png", 1, 1_000);
        let big = asset(&c, "大.png", 60 * 1024 * 1024, now_ms());
        let small = asset(&c, "小.png", 10, now_ms());

        let ids = eval_smart(&c, db::SMART_BIG_50MB, &[]).unwrap();
        assert_eq!(ids, vec![big], "只挑出大于 50 MB 的");
        let ids = eval_smart(&c, db::SMART_NEW_30D, &[]).unwrap();
        assert!(ids.contains(&big) && ids.contains(&small) && !ids.contains(&old));
        // 都还没进任何分组 → 未分类
        let ids = eval_smart(&c, db::SMART_UNTAGGED, &[]).unwrap();
        assert_eq!(ids.len(), 3);
        // duplicates 规则用的是外部传入的同源结果
        let ids = eval_smart(&c, db::SMART_DUPLICATES, &[big, old]).unwrap();
        assert_eq!(ids, vec![big, old]);
        assert!(eval_smart(&c, r#"{"kind":"nope"}"#, &[]).is_err());
    }

    #[test]
    fn 分组增删改与成员关系() {
        let c = conn("crud");
        let a = asset(&c, "a.png", 1, 1);
        let b = asset(&c, "b.png", 1, 1);
        let g = create(&c, "旅行").unwrap();
        assert_eq!(add_members(&c, g, &[a, b]).unwrap(), 2);
        let all = list(&c).unwrap();
        assert_eq!(all.iter().find(|x| x.id == g).unwrap().member_count, 2);

        rename(&c, g, "旅行 2026").unwrap();
        assert!(list(&c).unwrap().iter().any(|x| x.name == "旅行 2026"));
        assert_eq!(remove_members(&c, g, &[a]).unwrap(), 1);
        assert_eq!(list(&c).unwrap().iter().find(|x| x.id == g).unwrap().member_count, 1);

        // 同名分组会被拒绝
        assert!(create(&c, "旅行 2026").is_err());
        remove(&c, g).unwrap();
        assert!(!list(&c).unwrap().iter().any(|x| x.id == g));
    }

    #[test]
    fn 删分组不影响保护区() {
        let c = conn("indep");
        let a = asset(&c, "a.png", 1, 1);
        let g = create(&c, "临时").unwrap();
        add_members(&c, g, &[a]).unwrap();
        protect(&c, &[a], "manual", Some("手工加入")).unwrap();

        remove(&c, g).unwrap();
        assert_eq!(db::protected_count(&c).unwrap(), 1, "分组删了，保护区条目必须还在");
        assert!(db::is_protected_asset(&c, a));
    }

    #[test]
    fn 保护区增删查与当周统计() {
        let c = conn("protect");
        let a = asset(&c, "a.png", 1, 1);
        let b = asset(&c, "b.png", 1, 1);
        assert_eq!(protect(&c, &[a, b], "manual", None).unwrap(), 2);
        assert_eq!(protect(&c, &[a], "manual", None).unwrap(), 0, "重复加入不重复计数");

        let (total, week) = stats(&c).unwrap();
        assert_eq!(total, 2);
        assert_eq!(week, 2, "刚加入的两条都落在当周窗口内");

        let rows = list_protected(&c).unwrap();
        assert_eq!(rows.len(), 2);
        assert!(rows.iter().all(|r| r.added_by == "manual" && r.added_at > 0));

        assert_eq!(unprotect(&c, &[a]).unwrap(), 1);
        assert_eq!(db::protected_count(&c).unwrap(), 1);
        assert_eq!(unprotect_all(&c).unwrap(), 1);
        assert_eq!(db::protected_count(&c).unwrap(), 0);
    }

    #[test]
    fn 当周统计只数窗口内的条目() {
        let c = conn("week");
        let a = asset(&c, "a.png", 1, 1);
        protect(&c, &[a], "auto", Some("提交后自动加入")).unwrap();
        // 手工把 added_at 挪到 10 天前 → 不应再算当周新增
        c.execute(
            "UPDATE protections SET added_at = ?1",
            rusqlite::params![now_ms() - 10 * 24 * 60 * 60 * 1000],
        )
        .unwrap();
        let (total, week) = stats(&c).unwrap();
        assert_eq!(total, 1);
        assert_eq!(week, 0, "10 天前的条目不算当周新增");
    }
}
