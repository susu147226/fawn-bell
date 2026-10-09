//! `DraftUseCase`（执行版 §7.2 虚拟变更集 / §13.3 drafts 表）。
//!
//! 职责：把核心域算出来的草稿**落进库目录**（素材树零写入）、做实时预检、支持撤销 / 重做，
//! 并在启动时恢复上次未提交的草稿（§7.2「保存草稿并关闭」）。
//!
//! 安全要点：草稿是**唯一**允许改动的「将要发生」的状态；真实文件在提交前一个字节都不动
//! （§12.2 强制项①：只有 `CommitUseCase` 能写盘，P6 才做）。

use std::collections::HashSet;
use std::path::Path;
use std::sync::Mutex;

use rusqlite::Connection;

use crate::domain::drafts::{self, CheckStatus, Draft, DraftOp};
use crate::infra::db::{self, DraftRow};
use crate::infra::library::LibraryLayout;

/// 运行期草稿状态（UI 通过命令读写它）。
#[derive(Default)]
pub struct DraftState(Mutex<Handle>);

#[derive(Default)]
struct Handle {
    set: drafts::ChangeSet,
    loaded: bool,
}

impl DraftState {
    pub fn new() -> Self {
        Self::default()
    }

    /// 首次访问时从库里恢复草稿（只做一次）。
    pub fn ensure_loaded(&self, conn: &Connection) -> Result<(), String> {
        let mut h = self.0.lock().map_err(|_| "草稿状态已损坏，请重启鹿铃。".to_string())?;
        if h.loaded {
            return Ok(());
        }
        h.set = load_set(conn)?;
        h.loaded = true;
        Ok(())
    }

    pub fn snapshot(&self) -> Result<Vec<Draft>, String> {
        let h = self.0.lock().map_err(|_| "草稿状态已损坏，请重启鹿铃。".to_string())?;
        Ok(h.set.all().to_vec())
    }

    pub fn len(&self) -> Result<usize, String> {
        let h = self.0.lock().map_err(|_| "草稿状态已损坏，请重启鹿铃。".to_string())?;
        Ok(h.set.len())
    }

    pub fn problems(&self) -> Result<usize, String> {
        let h = self.0.lock().map_err(|_| "草稿状态已损坏，请重启鹿铃。".to_string())?;
        Ok(h.set.problems())
    }

    /// 重做栈里还有几条（状态条的重做按钮据此启用/禁用）。
    pub fn redo_len(&self) -> Result<usize, String> {
        let h = self.0.lock().map_err(|_| "草稿状态已损坏，请重启鹿铃。".to_string())?;
        Ok(h.set.redo_len())
    }

    pub fn add(&self, conn: &Connection, draft: Draft) -> Result<Draft, String> {
        let mut h = self.0.lock().map_err(|_| "草稿状态已损坏，请重启鹿铃。".to_string())?;
        if !h.loaded {
            h.set = load_set(conn)?;
            h.loaded = true;
        }
        let mut draft = draft;
        let (check, reason) = preflight(conn, &draft)?;
        draft.check = check;
        draft.reason = reason;
        h.set.add(draft.clone());
        let all: Vec<Draft> = h.set.all().to_vec();
        save_set(conn, &all)?;
        Ok(draft)
    }

    pub fn undo(&self, conn: &Connection) -> Result<Option<Draft>, String> {
        let mut h = self.0.lock().map_err(|_| "草稿状态已损坏，请重启鹿铃。".to_string())?;
        let undone = h.set.undo();
        let all: Vec<Draft> = h.set.all().to_vec();
        save_set(conn, &all)?;
        Ok(undone)
    }

    pub fn redo(&self, conn: &Connection) -> Result<Option<Draft>, String> {
        let mut h = self.0.lock().map_err(|_| "草稿状态已损坏，请重启鹿铃。".to_string())?;
        let redone = h.set.redo();
        let all: Vec<Draft> = h.set.all().to_vec();
        save_set(conn, &all)?;
        Ok(redone)
    }

    pub fn clear(&self, conn: &Connection) -> Result<(), String> {
        let mut h = self.0.lock().map_err(|_| "草稿状态已损坏，请重启鹿铃。".to_string())?;
        h.set.clear();
        db::clear_drafts(conn)
    }

    /// 投影：把一批真实路径折算成界面该显示的样子（§7.2）。
    pub fn project(&self, paths: &[String]) -> Result<Vec<drafts::Projection>, String> {
        let h = self.0.lock().map_err(|_| "草稿状态已损坏，请重启鹿铃。".to_string())?;
        Ok(paths.iter().map(|p| drafts::project(p, h.set.all())).collect())
    }
}

/// 草稿落库（覆盖式）。
pub fn save_set(conn: &Connection, all: &[Draft]) -> Result<(), String> {
    let rows: Vec<DraftRow> = all
        .iter()
        .map(|d| DraftRow {
            seq: d.seq as i64,
            op: d.op.as_str().to_string(),
            asset_id: d.asset_id,
            src: d.src.clone(),
            dst: d.dst.clone(),
            check_status: status_str(d.check).to_string(),
            check_reason: d.reason.clone(),
        })
        .collect();
    db::replace_drafts(conn, &rows)
}

/// 从库里恢复（§7.2：下次启动时恢复并提示「上次有 N 项未提交变更」）。
pub fn load_set(conn: &Connection) -> Result<drafts::ChangeSet, String> {
    let rows = db::load_drafts(conn)?;
    let mut set = drafts::ChangeSet::default();
    for r in rows {
        let op = DraftOp::parse(&r.op).unwrap_or(DraftOp::Rename);
        // 用 add 保持 seq 单调递增的内部计数：先把 next_seq 推到 max，再逐条 add
        let mut d = Draft::new(op, r.src, r.dst);
        d.asset_id = r.asset_id;
        d.check = status_of(&r.check_status);
        d.reason = r.check_reason;
        set.add(d);
    }
    Ok(set)
}

fn status_str(s: CheckStatus) -> &'static str {
    match s {
        CheckStatus::Ok => "ok",
        CheckStatus::Conflict => "conflict",
        CheckStatus::Illegal => "illegal",
        CheckStatus::TooLong => "too_long",
        CheckStatus::OutOfRoot => "out_of_root",
        CheckStatus::Protected => "protected",
        CheckStatus::Referenced => "referenced",
    }
}

fn status_of(s: &str) -> CheckStatus {
    match s {
        "conflict" => CheckStatus::Conflict,
        "illegal" => CheckStatus::Illegal,
        "too_long" => CheckStatus::TooLong,
        "out_of_root" => CheckStatus::OutOfRoot,
        "protected" => CheckStatus::Protected,
        "referenced" => CheckStatus::Referenced,
        _ => CheckStatus::Ok,
    }
}

/// 实时预检（§7.2）：名字非法 → 路径超长 → 越界 → 受保护 → 被引用 → 冲突。
///
/// 顺序即优先级：越靠前的结论越基础，避免把「名字非法」报成「冲突」。
pub fn preflight(conn: &Connection, draft: &Draft) -> Result<(CheckStatus, Option<String>), String> {
    if let Some(dst) = &draft.dst {
        let name = dst
            .rsplit(['\\', '/'])
            .next()
            .unwrap_or(dst)
            .to_string();
        if draft.op.renames() || draft.op == DraftOp::Mkdir {
            if let Some(why) = drafts::check_file_name(&name) {
                return Ok((CheckStatus::Illegal, Some(why)));
            }
        }
        if let Some(why) = drafts::check_path_len(dst) {
            return Ok((CheckStatus::TooLong, Some(why)));
        }
        // 越界：目标必须落在某个已登记的工作根之内（§7.2 预检清单「工作根内」）。
        let roots = db::scan_roots_all(conn)?;
        if !roots.is_empty() {
            let inside = roots.iter().any(|r| {
                crate::domain::guard::is_under(Path::new(r), Path::new(dst))
            });
            if !inside {
                return Ok((
                    CheckStatus::OutOfRoot,
                    Some(format!("`{dst}` 不在任何已扫描的素材根之内")),
                ));
            }
        }
    }

    if let Some(id) = draft.asset_id {
        if db::is_protected_asset(conn, id) {
            return Ok((
                CheckStatus::Protected,
                Some("该条目在保护区内，默认不参与操作".to_string()),
            ));
        }
    }

    // 被引用（§6.3.1）：只看目标文件名是否出现在引用映射里
    if let Some(dst) = &draft.dst {
        let name = dst.rsplit(['\\', '/']).next().unwrap_or(dst);
        let n = db::ref_count(conn, name)?;
        if n > 0 && draft.op.renames() {
            let src_name = draft.src.rsplit(['\\', '/']).next().unwrap_or(&draft.src);
            if n > 0 && name != src_name {
                return Ok((
                    CheckStatus::Referenced,
                    Some(format!("新名字 `{name}` 被 {n} 处引用命中，提交前请确认")),
                ));
            }
        }
    }

    // 冲突：目标在磁盘上已存在，或与另一条草稿撞名
    let all = db::load_drafts(conn)?;
    let others: Vec<Draft> = all
        .iter()
        .filter(|r| r.seq != draft.seq as i64)
        .map(|r| {
            let op = DraftOp::parse(&r.op).unwrap_or(DraftOp::Rename);
            let mut d = Draft::new(op, r.src.clone(), r.dst.clone());
            d.seq = r.seq as u64;
            d
        })
        .collect();

    if let Some(dst) = &draft.dst {
        // 与磁盘比：用元数据判断（不写盘）
        if Path::new(dst).exists() {
            return Ok((
                CheckStatus::Conflict,
                Some(format!("`{dst}` 在磁盘上已存在")),
            ));
        }
    }

    let mut combined = others;
    combined.push(draft.clone());
    let mut existing: HashSet<String> = HashSet::new();
    existing.insert(String::new()); // 占位，避免空集被优化掉语义
    let issues = drafts::detect_conflicts(&combined, &HashSet::new());
    if let Some((_, _, why)) = issues.into_iter().find(|(seq, _, _)| *seq == draft.seq) {
        return Ok((CheckStatus::Conflict, Some(why)));
    }

    Ok((CheckStatus::Ok, None))
}

/// 打开库并交给闭包用（命令层薄封装，避免每处都重复 open）。
pub fn with_library<T>(layout: &LibraryLayout, f: impl FnOnce(&Connection) -> Result<T, String>) -> Result<T, String> {
    let conn = db::open_library(layout)?;
    f(&conn)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::path::PathBuf;

    fn scene(name: &str) -> (PathBuf, LibraryLayout) {
        let base = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("target")
            .join("luling-tests")
            .join("draftusecase")
            .join(name);
        let _ = fs::remove_dir_all(&base);
        let root = base.join("素材");
        fs::create_dir_all(&root).unwrap();
        (root, LibraryLayout::for_root(base.join("库")))
    }

    #[test]
    fn 草稿落库并能在新状态下恢复() {
        let (root, layout) = scene("persist");
        let conn = db::open_library(&layout).unwrap();
        db::upsert_scan_root(&conn, &root.to_string_lossy(), false, None, 1).unwrap();

        let state = DraftState::new();
        let d = Draft::new(
            DraftOp::Rename,
            root.join("a.png").to_string_lossy(),
            Some(root.join("b.png").to_string_lossy().to_string()),
        );
        let added = state.add(&conn, d).unwrap();
        assert_eq!(added.check, CheckStatus::Ok, "{:?}", added.reason);
        assert_eq!(state.len().unwrap(), 1);

        // 新进程状态（模拟重启）→ 从库里恢复
        let fresh = DraftState::new();
        fresh.ensure_loaded(&conn).unwrap();
        assert_eq!(fresh.len().unwrap(), 1);
        assert_eq!(fresh.snapshot().unwrap()[0].dst.as_deref().unwrap().ends_with("b.png"), true);
    }

    #[test]
    fn 撤销重做会同步到库里() {
        let (root, layout) = scene("undo");
        let conn = db::open_library(&layout).unwrap();
        let state = DraftState::new();
        state.ensure_loaded(&conn).unwrap();

        state
            .add(&conn, Draft::new(DraftOp::Rename, root.join("a.png").to_string_lossy(), Some(root.join("b.png").to_string_lossy().to_string())))
            .unwrap();
        state
            .add(&conn, Draft::new(DraftOp::Rename, root.join("c.png").to_string_lossy(), Some(root.join("d.png").to_string_lossy().to_string())))
            .unwrap();
        assert_eq!(db::load_drafts(&conn).unwrap().len(), 2);

        state.undo(&conn).unwrap();
        assert_eq!(db::load_drafts(&conn).unwrap().len(), 1);
        state.redo(&conn).unwrap();
        assert_eq!(db::load_drafts(&conn).unwrap().len(), 2);
        state.clear(&conn).unwrap();
        assert_eq!(db::load_drafts(&conn).unwrap().len(), 0);
    }

    #[test]
    fn 预检_名字非法() {
        let (root, layout) = scene("illegal");
        let conn = db::open_library(&layout).unwrap();
        let state = DraftState::new();
        let bad = state
            .add(
                &conn,
                Draft::new(
                    DraftOp::Rename,
                    root.join("a.png").to_string_lossy(),
                    Some(root.join("a:b.png").to_string_lossy().to_string()),
                ),
            )
            .unwrap();
        assert_eq!(bad.check, CheckStatus::Illegal);
        assert!(bad.reason.unwrap_or_default().contains("不能有"));
    }

    #[test]
    fn 预检_越界与超长() {
        let (root, layout) = scene("outside");
        let conn = db::open_library(&layout).unwrap();
        db::upsert_scan_root(&conn, &root.to_string_lossy(), false, None, 1).unwrap();
        let state = DraftState::new();

        let out = state
            .add(
                &conn,
                Draft::new(
                    DraftOp::Move,
                    root.join("a.png").to_string_lossy(),
                    Some(r"D:\完全不在根里\a.png".to_string()),
                ),
            )
            .unwrap();
        assert_eq!(out.check, CheckStatus::OutOfRoot);

        let long_dst = root.join(format!("{}.png", "x".repeat(300))).to_string_lossy().to_string();
        let long = state
            .add(&conn, Draft::new(DraftOp::Rename, root.join("b.png").to_string_lossy(), Some(long_dst)))
            .unwrap();
        assert!(matches!(long.check, CheckStatus::TooLong | CheckStatus::Illegal));
    }

    #[test]
    fn 预检_磁盘已存在判为冲突() {
        let (root, layout) = scene("conflict");
        let conn = db::open_library(&layout).unwrap();
        db::upsert_scan_root(&conn, &root.to_string_lossy(), false, None, 1).unwrap();
        let target = root.join("存在.png");
        fs::write(&target, b"x").unwrap();

        let state = DraftState::new();
        let d = state
            .add(
                &conn,
                Draft::new(
                    DraftOp::Rename,
                    root.join("别的.png").to_string_lossy(),
                    Some(target.to_string_lossy().to_string()),
                ),
            )
            .unwrap();
        assert_eq!(d.check, CheckStatus::Conflict);
    }

    #[test]
    fn 预检_受保护条目被拦下() {
        let (root, layout) = scene("protected");
        let conn = db::open_library(&layout).unwrap();
        let id = db::upsert_asset(
            &conn,
            &db::AssetRecord {
                volume_id: "v".into(),
                rel_path: "a.png".into(),
                name: "a.png".into(),
                kind: "image".into(),
                size: 1,
                ..Default::default()
            },
        )
        .unwrap();
        conn.execute("INSERT INTO protections(asset_id, added_by, added_at) VALUES(?1,'auto',1)", rusqlite::params![id])
            .unwrap();

        let state = DraftState::new();
        let mut d = Draft::new(
            DraftOp::Rename,
            root.join("a.png").to_string_lossy(),
            Some(root.join("b.png").to_string_lossy().to_string()),
        );
        d.asset_id = Some(id);
        let added = state.add(&conn, d).unwrap();
        assert_eq!(added.check, CheckStatus::Protected);
    }

    #[test]
    fn 投影按草稿链给出界面显示值() {
        let (root, layout) = scene("project");
        let conn = db::open_library(&layout).unwrap();
        let state = DraftState::new();
        state.ensure_loaded(&conn).unwrap();
        let a = root.join("a.png").to_string_lossy().to_string();
        let b = root.join("b.png").to_string_lossy().to_string();
        state.add(&conn, Draft::new(DraftOp::Rename, a.clone(), Some(b.clone()))).unwrap();

        let projected = state.project(&[a.clone(), root.join("z.png").to_string_lossy().to_string()]).unwrap();
        assert_eq!(projected[0].path, b);
        assert!(projected[0].drafted);
        assert!(!projected[1].drafted);
    }
}
