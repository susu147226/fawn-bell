//! 基础设施层 · SQLite 仓储（执行版 §13 数据模型）。
//!
//! - 库结构由 [`migrate`] 一次建齐（§13.1–13.3 的全部表 + 用户批准的追加）：
//!   `assets` 增加 `gps_lat / gps_lon / orientation`（§6.4 要求解析 GPS 与方向），
//!   新增 `asset_meta`（EXIF / Shell 扩展属性的键值表，**不是真源**、可随时重建——
//!   §6.4 要求视频时长码率、音频艺术家、文档标题作者等，§13.2 的表里没有对应列），
//!   新增 `refs`（§7.1：扫描期建立的「文件名 → 引用它的文件」映射，§13.2 允许 P1 落地）。
//! - 一切都是库内文件，**没有任何一张表落在素材目录**（§14 第 1 条）。

use std::collections::{HashMap, HashSet};
use std::path::Path;

use rusqlite::{params, Connection, OptionalExtension};

use super::library::LibraryLayout;

/// 库结构版本（迁移器据此升级）。
pub const SCHEMA_VERSION: i64 = 1;

/// 建表语句（§13.1–13.3 全量；P1 只用其中一部分，但结构一次到位避免反复迁移）。
const SCHEMA_SQL: &str = r#"
-- 元信息
CREATE TABLE IF NOT EXISTS schema_meta (
  key TEXT PRIMARY KEY, value TEXT NOT NULL
);

-- §13.1 工作根
CREATE TABLE IF NOT EXISTS scan_roots (
  id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE,
  include_glob TEXT, exclude_glob TEXT,
  follow_links INTEGER DEFAULT 0, max_depth INTEGER,
  last_scan_at INTEGER
);

-- §13.2 素材索引（磁盘真实状态的缓存）
CREATE TABLE IF NOT EXISTS assets (
  id INTEGER PRIMARY KEY,
  volume_id TEXT NOT NULL, rel_path TEXT NOT NULL COLLATE NOCASE,
  name TEXT NOT NULL, ext TEXT, kind TEXT,
  size INTEGER, mtime INTEGER, ctime INTEGER,
  width INTEGER, height INTEGER,
  capture_time INTEGER, camera TEXT,
  gps_lat REAL, gps_lon REAL, orientation INTEGER,
  hash_partial TEXT,
  missing INTEGER DEFAULT 0,
  UNIQUE(volume_id, rel_path)
);
CREATE INDEX IF NOT EXISTS idx_assets_hash ON assets(size, hash_partial);
CREATE INDEX IF NOT EXISTS idx_assets_kind ON assets(kind);
CREATE INDEX IF NOT EXISTS idx_assets_missing ON assets(missing);

-- §13.2 扩展属性（EXIF / Shell；键值表，可重建）
CREATE TABLE IF NOT EXISTS asset_meta (
  asset_id INTEGER NOT NULL, key TEXT NOT NULL, value TEXT NOT NULL,
  PRIMARY KEY(asset_id, key)
);

-- §7.1 / §13.2 引用映射（扫描期建立，可随时重建）
CREATE TABLE IF NOT EXISTS refs (
  ref_name TEXT NOT NULL, holder_asset_id INTEGER,
  holder_path TEXT NOT NULL, line_no INTEGER,
  UNIQUE(ref_name, holder_path, line_no)
);
CREATE INDEX IF NOT EXISTS idx_refs_name ON refs(ref_name);

-- §13.2 已整理标记
CREATE TABLE IF NOT EXISTS organized (
  asset_id INTEGER PRIMARY KEY,
  first_batch_id TEXT, last_batch_id TEXT,
  organized_at INTEGER, times INTEGER DEFAULT 1
);

-- §13.2 保护区
CREATE TABLE IF NOT EXISTS protections (
  asset_id INTEGER PRIMARY KEY,
  added_by TEXT, added_at INTEGER, reason TEXT
);

-- §13.2 分组
CREATE TABLE IF NOT EXISTS groups (
  id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE,
  kind TEXT NOT NULL, rule_json TEXT, color TEXT,
  parent_id INTEGER, sort_order INTEGER
);
CREATE TABLE IF NOT EXISTS asset_group (
  asset_id INTEGER NOT NULL, group_id INTEGER NOT NULL,
  PRIMARY KEY(asset_id, group_id)
);

-- §13.3 虚拟变更集（草稿）
CREATE TABLE IF NOT EXISTS drafts (
  id INTEGER PRIMARY KEY,
  seq INTEGER NOT NULL,
  op TEXT NOT NULL,
  asset_id INTEGER,
  src TEXT, dst TEXT,
  payload_json TEXT,
  check_status TEXT,
  check_reason TEXT,
  created_at INTEGER
);
CREATE INDEX IF NOT EXISTS idx_drafts_seq ON drafts(seq);

-- §13.3 规则方案
CREATE TABLE IF NOT EXISTS rules (
  id INTEGER PRIMARY KEY, name TEXT NOT NULL,
  scope_group_id INTEGER,
  match_json TEXT NOT NULL, rename_template TEXT, target_dir_template TEXT,
  conflict_policy TEXT, seq_json TEXT,
  priority INTEGER DEFAULT 0, is_preset INTEGER DEFAULT 0
);

-- §13.3 命名模板预设
CREATE TABLE IF NOT EXISTS name_presets (
  id INTEGER PRIMARY KEY,
  base_name TEXT NOT NULL,
  label TEXT,
  template TEXT NOT NULL,
  is_builtin INTEGER DEFAULT 0,
  sort_order INTEGER, created_at INTEGER
);

-- §13.3 提交批次与操作日志
CREATE TABLE IF NOT EXISTS batches (
  id TEXT PRIMARY KEY, kind TEXT, plan_json TEXT, summary_json TEXT,
  status TEXT, started_at INTEGER, finished_at INTEGER, note TEXT
);
CREATE TABLE IF NOT EXISTS ops (
  id INTEGER PRIMARY KEY, batch_id TEXT NOT NULL, seq INTEGER NOT NULL,
  op TEXT NOT NULL, src TEXT, dst TEXT,
  before_state TEXT, after_state TEXT,
  status TEXT NOT NULL, error TEXT, created_at INTEGER
);
CREATE INDEX IF NOT EXISTS idx_ops_batch ON ops(batch_id, seq);

-- §13.3 设置 / 主题 / 浏览历史 / 素材树快照
CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT NOT NULL, updated_at INTEGER);
CREATE TABLE IF NOT EXISTS themes (
  id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE,
  tokens_json TEXT NOT NULL, is_builtin INTEGER DEFAULT 0, created_at INTEGER
);
CREATE TABLE IF NOT EXISTS nav_history (id INTEGER PRIMARY KEY, path TEXT, visited_at INTEGER);
CREATE TABLE IF NOT EXISTS tree_snapshots (
  id INTEGER PRIMARY KEY, root_id INTEGER NOT NULL,
  taken_at INTEGER, entries_json TEXT, note TEXT
);
"#;

/// 打开一个库文件（WAL、忙等待、外键开）。
pub fn open(db_path: &Path) -> Result<Connection, String> {
    if let Some(parent) = db_path.parent() {
        std::fs::create_dir_all(parent).map_err(|e| format!("建库目录失败：{e}"))?;
    }
    let conn = Connection::open(db_path).map_err(|e| format!("打开库失败：{e}"))?;
    conn.pragma_update(None, "journal_mode", "WAL")
        .map_err(|e| e.to_string())?;
    conn.pragma_update(None, "synchronous", "NORMAL")
        .map_err(|e| e.to_string())?;
    conn.pragma_update(None, "foreign_keys", "ON")
        .map_err(|e| e.to_string())?;
    conn.busy_timeout(std::time::Duration::from_millis(3000))
        .map_err(|e| e.to_string())?;
    Ok(conn)
}

/// 建表 / 升级结构（幂等）。
pub fn migrate(conn: &Connection) -> Result<(), String> {
    conn.execute_batch(SCHEMA_SQL).map_err(|e| format!("建表失败：{e}"))?;
    conn.execute(
        "INSERT INTO schema_meta(key, value) VALUES('schema_version', ?1)
         ON CONFLICT(key) DO UPDATE SET value = excluded.value",
        params![SCHEMA_VERSION.to_string()],
    )
    .map_err(|e| e.to_string())?;
    Ok(())
}

/// 建齐目录并打开库（首次真正落盘发生在这里）。
pub fn open_library(layout: &LibraryLayout) -> Result<Connection, String> {
    super::library::ensure(layout).map_err(|e| format!("建库目录失败：{e}"))?;
    let conn = open(&layout.db)?;
    migrate(&conn)?;
    Ok(conn)
}

fn now_ms() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as i64)
        .unwrap_or(0)
}

/* ── 设置 ─────────────────────────────────────────────────────────── */

pub fn setting_get(conn: &Connection, key: &str) -> Option<String> {
    conn.query_row(
        "SELECT value FROM settings WHERE key = ?1",
        params![key],
        |row| row.get::<_, String>(0),
    )
    .optional()
    .ok()
    .flatten()
}

pub fn setting_set(conn: &Connection, key: &str, value: &str) -> Result<(), String> {
    conn.execute(
        "INSERT INTO settings(key, value, updated_at) VALUES(?1, ?2, ?3)
         ON CONFLICT(key) DO UPDATE SET value = excluded.value, updated_at = excluded.updated_at",
        params![key, value, now_ms()],
    )
    .map_err(|e| e.to_string())?;
    Ok(())
}

/* ── 素材索引 ─────────────────────────────────────────────────────── */

/// 一条素材索引记录（字段与 §13.2 的 `assets` 对齐）。
#[derive(Debug, Clone, Default, PartialEq)]
pub struct AssetRecord {
    pub volume_id: String,
    pub rel_path: String,
    pub name: String,
    pub ext: Option<String>,
    pub kind: String,
    pub size: i64,
    pub mtime: i64,
    pub ctime: i64,
    pub width: Option<i64>,
    pub height: Option<i64>,
    pub capture_time: Option<i64>,
    pub camera: Option<String>,
    pub gps_lat: Option<f64>,
    pub gps_lon: Option<f64>,
    pub orientation: Option<i64>,
    pub hash_partial: Option<String>,
}

/// 增量判定所需的既有指纹。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Fingerprint {
    pub id: i64,
    pub size: i64,
    pub mtime: i64,
    pub ctime: i64,
    /// 已经算过内容指纹（首尾各 64 KB）的标记：为空表示还没算。
    pub has_hash: bool,
}

impl Fingerprint {
    /// §7.1：按「体积 + 修改时间 + 创建时间」判定是否变更。
    pub fn unchanged(&self, size: i64, mtime: i64, ctime: i64) -> bool {
        self.size == size && self.mtime == mtime && self.ctime == ctime
    }
}

/// 读取某个卷上的全部索引（一次性进内存，避免逐文件查库）。
pub fn load_volume_index(conn: &Connection, volume_id: &str) -> Result<HashMap<String, Fingerprint>, String> {
    let mut stmt = conn
        .prepare(
            "SELECT id, rel_path, size, mtime, ctime, hash_partial IS NOT NULL
             FROM assets WHERE volume_id = ?1",
        )
        .map_err(|e| e.to_string())?;
    let rows = stmt
        .query_map(params![volume_id], |row| {
            Ok((
                row.get::<_, String>(1)?,
                Fingerprint {
                    id: row.get(0)?,
                    size: row.get(2)?,
                    mtime: row.get(3)?,
                    ctime: row.get(4)?,
                    has_hash: row.get::<_, i64>(5)? != 0,
                },
            ))
        })
        .map_err(|e| e.to_string())?;
    let mut map = HashMap::new();
    for row in rows {
        let (rel, fp) = row.map_err(|e| e.to_string())?;
        map.insert(rel, fp);
    }
    Ok(map)
}

/// 写入或更新一条素材（按 `(volume_id, rel_path)` 归并；返回资产 id）。
pub fn upsert_asset(conn: &Connection, rec: &AssetRecord) -> Result<i64, String> {
    conn.execute(
        "INSERT INTO assets(volume_id, rel_path, name, ext, kind, size, mtime, ctime,
                            width, height, capture_time, camera, gps_lat, gps_lon, orientation,
                            hash_partial, missing)
         VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, 0)
         ON CONFLICT(volume_id, rel_path) DO UPDATE SET
           name = excluded.name, ext = excluded.ext, kind = excluded.kind,
           size = excluded.size, mtime = excluded.mtime, ctime = excluded.ctime,
           width = excluded.width, height = excluded.height,
           capture_time = excluded.capture_time, camera = excluded.camera,
           gps_lat = excluded.gps_lat, gps_lon = excluded.gps_lon,
           orientation = excluded.orientation,
           hash_partial = COALESCE(excluded.hash_partial, assets.hash_partial),
           missing = 0",
        params![
            rec.volume_id,
            rec.rel_path,
            rec.name,
            rec.ext,
            rec.kind,
            rec.size,
            rec.mtime,
            rec.ctime,
            rec.width,
            rec.height,
            rec.capture_time,
            rec.camera,
            rec.gps_lat,
            rec.gps_lon,
            rec.orientation,
            rec.hash_partial,
        ],
    )
    .map_err(|e| e.to_string())?;
    conn.query_row(
        "SELECT id FROM assets WHERE volume_id = ?1 AND rel_path = ?2",
        params![rec.volume_id, rec.rel_path],
        |row| row.get(0),
    )
    .map_err(|e| e.to_string())
}

/// 把本次扫描没看见的条目标记为 `missing`（§7.1：**不自动清理**元数据）。
///
/// 返回被标记的条数。
pub fn mark_missing(
    conn: &Connection,
    volume_id: &str,
    seen_rel_paths: &HashSet<String>,
) -> Result<usize, String> {
    let existing = load_volume_index(conn, volume_id)?;
    let seen: HashSet<String> = seen_rel_paths.iter().map(|s| s.to_lowercase()).collect();
    let mut marked = 0usize;
    for (rel, fp) in existing {
        if seen.contains(&rel.to_lowercase()) {
            continue;
        }
        conn.execute("UPDATE assets SET missing = 1 WHERE id = ?1", params![fp.id])
            .map_err(|e| e.to_string())?;
        marked += 1;
    }
    Ok(marked)
}

/// 把**这次又看见了**的条目从 `missing` 恢复（§7.1 的对称操作）。
///
/// 为什么需要它：增量扫描会整条跳过「体积/时间都没变」的条目，于是这些条目的 `missing`
/// 标记不会被 `upsert_asset` 顺手清掉——素材树搬回来、或先扫了别的根把它标成缺失之后再扫回本根，
/// 就会出现「文件在、索引却说是缺失」的假象。返回恢复条数。
pub fn clear_missing(
    conn: &Connection,
    volume_id: &str,
    seen_rel_paths: &HashSet<String>,
) -> Result<usize, String> {
    let seen: HashSet<String> = seen_rel_paths.iter().map(|s| s.to_lowercase()).collect();
    let mut stmt = conn
        .prepare("SELECT id, rel_path FROM assets WHERE volume_id = ?1 AND missing = 1")
        .map_err(|e| e.to_string())?;
    let rows = stmt
        .query_map(params![volume_id], |row| {
            Ok((row.get::<_, i64>(0)?, row.get::<_, String>(1)?))
        })
        .map_err(|e| e.to_string())?;
    let mut recovered = Vec::new();
    for row in rows {
        let (id, rel) = row.map_err(|e| e.to_string())?;
        if seen.contains(&rel.to_lowercase()) {
            recovered.push(id);
        }
    }
    drop(stmt);
    let mut n = 0usize;
    for id in recovered {
        conn.execute("UPDATE assets SET missing = 0 WHERE id = ?1", params![id])
            .map_err(|e| e.to_string())?;
        n += 1;
    }
    Ok(n)
}

/// 某个卷上被标记为 `missing` 的条目（§7.1：灰色显示 + 两个动作）。
pub fn missing_assets(conn: &Connection, volume_id: &str) -> Result<Vec<(i64, String, String)>, String> {
    let mut stmt = conn
        .prepare("SELECT id, rel_path, name FROM assets WHERE volume_id = ?1 AND missing = 1")
        .map_err(|e| e.to_string())?;
    let rows = stmt
        .query_map(params![volume_id], |row| {
            Ok((row.get(0)?, row.get(1)?, row.get(2)?))
        })
        .map_err(|e| e.to_string())?;
    let mut out = Vec::new();
    for r in rows {
        out.push(r.map_err(|e| e.to_string())?);
    }
    Ok(out)
}

/// 从索引移除若干条目（§7.1「从索引移除」动作，只删索引，不碰磁盘）。
pub fn forget_assets(conn: &Connection, ids: &[i64]) -> Result<usize, String> {
    let mut n = 0usize;
    for id in ids {
        conn.execute("DELETE FROM asset_meta WHERE asset_id = ?1", params![id])
            .map_err(|e| e.to_string())?;
        conn.execute("DELETE FROM assets WHERE id = ?1", params![id])
            .map_err(|e| e.to_string())?;
        n += 1;
    }
    Ok(n)
}

pub fn count_assets(conn: &Connection) -> Result<i64, String> {
    conn.query_row("SELECT COUNT(*) FROM assets", [], |row| row.get(0))
        .map_err(|e| e.to_string())
}

/* ── 扩展属性（EXIF / Shell） ─────────────────────────────────────── */

pub fn meta_set(conn: &Connection, asset_id: i64, key: &str, value: &str) -> Result<(), String> {
    conn.execute(
        "INSERT INTO asset_meta(asset_id, key, value) VALUES(?1, ?2, ?3)
         ON CONFLICT(asset_id, key) DO UPDATE SET value = excluded.value",
        params![asset_id, key, value],
    )
    .map_err(|e| e.to_string())?;
    Ok(())
}

pub fn meta_all(conn: &Connection, asset_id: i64) -> Result<Vec<(String, String)>, String> {
    let mut stmt = conn
        .prepare("SELECT key, value FROM asset_meta WHERE asset_id = ?1 ORDER BY key")
        .map_err(|e| e.to_string())?;
    let rows = stmt
        .query_map(params![asset_id], |row| Ok((row.get(0)?, row.get(1)?)))
        .map_err(|e| e.to_string())?;
    let mut out = Vec::new();
    for r in rows {
        out.push(r.map_err(|e| e.to_string())?);
    }
    Ok(out)
}

/* ── 工作根 ───────────────────────────────────────────────────────── */

pub fn upsert_scan_root(
    conn: &Connection,
    path: &str,
    follow_links: bool,
    max_depth: Option<i64>,
    scanned_at: i64,
) -> Result<i64, String> {
    conn.execute(
        "INSERT INTO scan_roots(path, follow_links, max_depth, last_scan_at)
         VALUES(?1, ?2, ?3, ?4)
         ON CONFLICT(path) DO UPDATE SET
           follow_links = excluded.follow_links,
           max_depth = excluded.max_depth,
           last_scan_at = excluded.last_scan_at",
        params![path, follow_links as i64, max_depth, scanned_at],
    )
    .map_err(|e| e.to_string())?;
    conn.query_row("SELECT id FROM scan_roots WHERE path = ?1", params![path], |row| {
        row.get(0)
    })
    .map_err(|e| e.to_string())
}

/// 引用映射：重建某个「持有文件」的全部引用行（先删后插，保证与磁盘一致）。
pub fn replace_refs(
    conn: &Connection,
    holder_path: &str,
    holder_asset_id: Option<i64>,
    refs: &[(String, i64)],
) -> Result<usize, String> {
    conn.execute("DELETE FROM refs WHERE holder_path = ?1", params![holder_path])
        .map_err(|e| e.to_string())?;
    let mut n = 0usize;
    for (name, line) in refs {
        conn.execute(
            "INSERT OR IGNORE INTO refs(ref_name, holder_asset_id, holder_path, line_no)
             VALUES(?1, ?2, ?3, ?4)",
            params![name, holder_asset_id, holder_path, line],
        )
        .map_err(|e| e.to_string())?;
        n += 1;
    }
    Ok(n)
}

/// 哪些持有文件引用了这个名字（P3 引用感知只做查表命中）。
pub fn refs_for(conn: &Connection, ref_name: &str) -> Result<Vec<(String, i64)>, String> {
    let mut stmt = conn
        .prepare("SELECT holder_path, line_no FROM refs WHERE ref_name = ?1")
        .map_err(|e| e.to_string())?;
    let rows = stmt
        .query_map(params![ref_name], |row| Ok((row.get(0)?, row.get(1)?)))
        .map_err(|e| e.to_string())?;
    let mut out = Vec::new();
    for r in rows {
        out.push(r.map_err(|e| e.to_string())?);
    }
    Ok(out)
}

/// 某个持有文件当前有多少条引用行（用于判断要不要重建映射）。
pub fn refs_count_for_holder(conn: &Connection, holder_path: &str) -> Result<i64, String> {
    conn.query_row(
        "SELECT COUNT(*) FROM refs WHERE holder_path = ?1",
        params![holder_path],
        |row| row.get(0),
    )
    .map_err(|e| e.to_string())
}

/* ── 内容去重（§7.12）：候选与「重复内容」智能集合 ───────────────── */

/// 去重候选行：第一级指纹（体积 + 首尾 64 KB 哈希）相同且不止一条的条目。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CandidateRow {
    pub asset_id: i64,
    pub volume_id: String,
    pub rel_path: String,
    pub size: i64,
    pub hash_partial: String,
    pub ctime: i64,
}

/// 取内容去重的候选（§7.12 第一级）：同体积 + 同分段哈希，且同组至少两条；缺失条目不参与。
pub fn duplicate_candidates(conn: &Connection) -> Result<Vec<CandidateRow>, String> {
    let mut stmt = conn
        .prepare(
            "SELECT a.id, a.volume_id, a.rel_path, a.size, a.hash_partial, COALESCE(a.ctime, 0)
             FROM assets a
             WHERE a.missing = 0 AND a.hash_partial IS NOT NULL
               AND EXISTS (
                 SELECT 1 FROM assets b
                 WHERE b.size = a.size AND b.hash_partial = a.hash_partial
                   AND b.id <> a.id AND b.missing = 0
               )
             ORDER BY a.size, a.hash_partial, a.id",
        )
        .map_err(|e| e.to_string())?;
    let rows = stmt
        .query_map([], |row| {
            Ok(CandidateRow {
                asset_id: row.get(0)?,
                volume_id: row.get(1)?,
                rel_path: row.get(2)?,
                size: row.get(3)?,
                hash_partial: row.get(4)?,
                ctime: row.get(5)?,
            })
        })
        .map_err(|e| e.to_string())?;
    let mut out = Vec::new();
    for r in rows {
        out.push(r.map_err(|e| e.to_string())?);
    }
    Ok(out)
}

/// 所有扫描过的工作根（用于把 `(volume_id, rel_path)` 还原成绝对路径）。
pub fn scan_roots_all(conn: &Connection) -> Result<Vec<String>, String> {
    let mut stmt = conn
        .prepare("SELECT path FROM scan_roots ORDER BY last_scan_at DESC, id DESC")
        .map_err(|e| e.to_string())?;
    let rows = stmt
        .query_map([], |row| row.get::<_, String>(0))
        .map_err(|e| e.to_string())?;
    let mut out = Vec::new();
    for r in rows {
        out.push(r.map_err(|e| e.to_string())?);
    }
    Ok(out)
}

/// 建立（或更新）一条智能集合并返回其 id（同名只保留一条）。
pub fn ensure_smart_group(conn: &Connection, name: &str, rule_json: &str) -> Result<i64, String> {
    conn.execute(
        "INSERT INTO groups(name, kind, rule_json, sort_order) VALUES(?1, 'smart', ?2, 0)
         ON CONFLICT(name) DO UPDATE SET kind = 'smart', rule_json = excluded.rule_json",
        params![name, rule_json],
    )
    .map_err(|e| e.to_string())?;
    conn.query_row("SELECT id FROM groups WHERE name = ?1", params![name], |row| row.get(0))
        .map_err(|e| e.to_string())
}

/// 用给定成员整体替换某个分组的成员（判定即集合，保证同源）。
pub fn replace_group_members(conn: &Connection, group_id: i64, asset_ids: &[i64]) -> Result<usize, String> {
    conn.execute("DELETE FROM asset_group WHERE group_id = ?1", params![group_id])
        .map_err(|e| e.to_string())?;
    let mut n = 0usize;
    for id in asset_ids {
        conn.execute(
            "INSERT OR IGNORE INTO asset_group(asset_id, group_id) VALUES(?1, ?2)",
            params![id, group_id],
        )
        .map_err(|e| e.to_string())?;
        n += 1;
    }
    Ok(n)
}

/// 某个分组的成员资产 id。
pub fn group_members(conn: &Connection, group_id: i64) -> Result<Vec<i64>, String> {
    let mut stmt = conn
        .prepare("SELECT asset_id FROM asset_group WHERE group_id = ?1 ORDER BY asset_id")
        .map_err(|e| e.to_string())?;
    let rows = stmt
        .query_map(params![group_id], |row| row.get::<_, i64>(0))
        .map_err(|e| e.to_string())?;
    let mut out = Vec::new();
    for r in rows {
        out.push(r.map_err(|e| e.to_string())?);
    }
    Ok(out)
}

/// 界面上「点开一个文件」需要的索引字段（§8.5 右栏详情 / §6.4 元数据）。
#[derive(Debug, Clone, PartialEq)]
pub struct AssetDetail {
    pub id: i64,
    pub width: Option<i64>,
    pub height: Option<i64>,
    pub capture_time: Option<i64>,
    pub camera: Option<String>,
    pub gps_lat: Option<f64>,
    pub gps_lon: Option<f64>,
    pub orientation: Option<i64>,
    pub missing: bool,
}

/// 按索引键 `(volume_id, rel_path)` 取一条素材的元数据；不在索引里返回 `None`。
pub fn asset_detail(
    conn: &Connection,
    volume_id: &str,
    rel_path: &str,
) -> Result<Option<AssetDetail>, String> {
    conn.query_row(
        "SELECT id, width, height, capture_time, camera, gps_lat, gps_lon, orientation, missing
         FROM assets WHERE volume_id = ?1 AND rel_path = ?2",
        params![volume_id, rel_path],
        |row| {
            Ok(AssetDetail {
                id: row.get(0)?,
                width: row.get(1)?,
                height: row.get(2)?,
                capture_time: row.get(3)?,
                camera: row.get(4)?,
                gps_lat: row.get(5)?,
                gps_lon: row.get(6)?,
                orientation: row.get(7)?,
                missing: row.get::<_, i64>(8)? != 0,
            })
        },
    )
    .optional()
    .map_err(|e| e.to_string())
}

/// 「重新定位素材树」用的一行索引（§13.4）。
#[derive(Debug, Clone, PartialEq)]
pub struct RelocateRow {
    pub id: i64,
    pub volume_id: String,
    pub rel_path: String,
    pub name: String,
    pub size: i64,
    pub mtime: i64,
}

/// 取某个卷上的全部索引行（重定位要拿全量比对）。
pub fn assets_of_volume(conn: &Connection, volume_id: &str) -> Result<Vec<RelocateRow>, String> {
    let mut stmt = conn
        .prepare(
            "SELECT id, volume_id, rel_path, name, COALESCE(size, 0), COALESCE(mtime, 0)
             FROM assets WHERE volume_id = ?1 ORDER BY id",
        )
        .map_err(|e| e.to_string())?;
    let rows = stmt
        .query_map(params![volume_id], |row| {
            Ok(RelocateRow {
                id: row.get(0)?,
                volume_id: row.get(1)?,
                rel_path: row.get(2)?,
                name: row.get(3)?,
                size: row.get(4)?,
                mtime: row.get(5)?,
            })
        })
        .map_err(|e| e.to_string())?;
    let mut out = Vec::new();
    for r in rows {
        out.push(r.map_err(|e| e.to_string())?);
    }
    Ok(out)
}

/// 重定位：改写一条索引的定位键（§13.4 高置信 / 待确认两档都走这里）。
pub fn rewrite_location(conn: &Connection, id: i64, volume_id: &str, rel_path: &str) -> Result<(), String> {
    conn.execute(
        "UPDATE assets SET volume_id = ?2, rel_path = ?3, missing = 0 WHERE id = ?1",
        params![id, volume_id, rel_path],
    )
    .map_err(|e| e.to_string())?;
    Ok(())
}

/* ── 草稿（虚拟变更集，§7.2 / §13.3） ─────────────────────────────── */

/// 草稿行（只含基本类型，不依赖核心域类型，便于 infra 保持独立）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DraftRow {
    pub seq: i64,
    pub op: String,
    pub asset_id: Option<i64>,
    pub src: String,
    pub dst: Option<String>,
    pub check_status: String,
    pub check_reason: Option<String>,
}

/// 覆盖式写入全部草稿（草稿量级小，整体替换最简单也最不容易出错）。
pub fn replace_drafts(conn: &Connection, rows: &[DraftRow]) -> Result<(), String> {
    let tx = conn.unchecked_transaction().map_err(|e| e.to_string())?;
    tx.execute("DELETE FROM drafts", []).map_err(|e| e.to_string())?;
    let now = now_ms();
    for r in rows {
        tx.execute(
            "INSERT INTO drafts(seq, op, asset_id, src, dst, payload_json, check_status, check_reason, created_at)
             VALUES(?1, ?2, ?3, ?4, ?5, NULL, ?6, ?7, ?8)",
            params![r.seq, r.op, r.asset_id, r.src, r.dst, r.check_status, r.check_reason, now],
        )
        .map_err(|e| e.to_string())?;
    }
    tx.commit().map_err(|e| e.to_string())?;
    Ok(())
}

/// 读回全部草稿（启动恢复用）。
pub fn load_drafts(conn: &Connection) -> Result<Vec<DraftRow>, String> {
    let mut stmt = conn
        .prepare("SELECT seq, op, asset_id, src, dst, check_status, check_reason FROM drafts ORDER BY seq")
        .map_err(|e| e.to_string())?;
    let rows = stmt
        .query_map([], |row| {
            Ok(DraftRow {
                seq: row.get(0)?,
                op: row.get(1)?,
                asset_id: row.get(2)?,
                src: row.get(3)?,
                dst: row.get(4)?,
                check_status: row.get(5)?,
                check_reason: row.get(6)?,
            })
        })
        .map_err(|e| e.to_string())?;
    let mut out = Vec::new();
    for r in rows {
        out.push(r.map_err(|e| e.to_string())?);
    }
    Ok(out)
}

pub fn clear_drafts(conn: &Connection) -> Result<(), String> {
    conn.execute("DELETE FROM drafts", []).map_err(|e| e.to_string())?;
    Ok(())
}

/// 该素材是否在保护区内（§7.6；保护区是独立表，不随分组删除而丢）。
pub fn is_protected_asset(conn: &Connection, asset_id: i64) -> bool {
    conn.query_row(
        "SELECT 1 FROM protections WHERE asset_id = ?1",
        params![asset_id],
        |_| Ok(()),
    )
    .optional()
    .ok()
    .flatten()
    .is_some()
}

/// 该名字被多少处引用（§6.3.1：预览里提示「该文件被 N 处引用」）。
pub fn ref_count(conn: &Connection, name: &str) -> Result<i64, String> {
    conn.query_row(
        "SELECT COUNT(*) FROM refs WHERE ref_name = ?1",
        params![name.to_lowercase()],
        |row| row.get(0),
    )
    .map_err(|e| e.to_string())
}

pub fn schema_version(conn: &Connection) -> Option<i64> {
    setting_get(conn, "__schema_version")
        .or_else(|| {
            conn.query_row(
                "SELECT value FROM schema_meta WHERE key = 'schema_version'",
                [],
                |row| row.get::<_, String>(0),
            )
            .optional()
            .ok()
            .flatten()
        })
        .and_then(|v| v.parse().ok())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::infra::library::LibraryLayout;

    fn temp_layout(name: &str) -> LibraryLayout {
        let dir = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("target")
            .join("luling-tests")
            .join(name);
        let _ = std::fs::remove_dir_all(&dir);
        LibraryLayout::for_root(dir.join("库"))
    }

    fn conn(name: &str) -> Connection {
        open_library(&temp_layout(name)).expect("开库")
    }

    #[test]
    fn 迁移建齐表并记录版本() {
        let c = conn("db-migrate");
        let n: i64 = c
            .query_row(
                "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'",
                [],
                |row| row.get(0),
            )
            .unwrap();
        // §13 全部表 + schema_meta + asset_meta + refs
        assert!(n >= 18, "表数量偏少：{n}");
        assert_eq!(schema_version(&c), Some(SCHEMA_VERSION));
    }

    #[test]
    fn 设置读写() {
        let c = conn("db-settings");
        assert_eq!(setting_get(&c, "storage.location"), None);
        setting_set(&c, "storage.location", "appData").unwrap();
        assert_eq!(setting_get(&c, "storage.location").as_deref(), Some("appData"));
        setting_set(&c, "storage.location", "portable").unwrap();
        assert_eq!(setting_get(&c, "storage.location").as_deref(), Some("portable"));
    }

    #[test]
    fn 素材写入并按卷路径归并() {
        let c = conn("db-assets");
        let mut rec = AssetRecord {
            volume_id: "vol-1".into(),
            rel_path: r"photos\a.jpg".into(),
            name: "a.jpg".into(),
            ext: Some("jpg".into()),
            kind: "image".into(),
            size: 100,
            mtime: 10,
            ctime: 5,
            width: Some(1920),
            height: Some(1080),
            ..Default::default()
        };
        let id1 = upsert_asset(&c, &rec).unwrap();
        rec.size = 200;
        rec.width = Some(3840);
        let id2 = upsert_asset(&c, &rec).unwrap();
        assert_eq!(id1, id2, "同卷同相对路径应归并为同一条");
        assert_eq!(count_assets(&c).unwrap(), 1);
        let (size, width): (i64, i64) = c
            .query_row("SELECT size, width FROM assets WHERE id = ?1", params![id1], |r| {
                Ok((r.get(0)?, r.get(1)?))
            })
            .unwrap();
        assert_eq!((size, width), (200, 3840));
    }

    #[test]
    fn 指纹判定只看体积修改时间创建时间() {
        let fp = Fingerprint { id: 1, size: 10, mtime: 20, ctime: 30, has_hash: false };
        assert!(fp.unchanged(10, 20, 30));
        assert!(!fp.unchanged(11, 20, 30));
        assert!(!fp.unchanged(10, 21, 30));
        assert!(!fp.unchanged(10, 20, 31));
    }

    #[test]
    fn 未再出现的条目被标记missing且不被删除() {
        let c = conn("db-missing");
        for rel in [r"a\1.jpg", r"a\2.jpg", r"a\3.jpg"] {
            upsert_asset(
                &c,
                &AssetRecord {
                    volume_id: "v".into(),
                    rel_path: rel.into(),
                    name: rel.rsplit('\\').next().unwrap().into(),
                    kind: "image".into(),
                    size: 1,
                    ..Default::default()
                },
            )
            .unwrap();
        }
        let mut seen = HashSet::new();
        seen.insert(r"a\1.jpg".to_string());
        seen.insert(r"a\3.jpg".to_string());
        let marked = mark_missing(&c, "v", &seen).unwrap();
        assert_eq!(marked, 1);
        let missing = missing_assets(&c, "v").unwrap();
        assert_eq!(missing.len(), 1);
        assert_eq!(missing[0].1, r"a\2.jpg");
        // 元数据仍在（§7.1：不自动清理）
        assert_eq!(count_assets(&c).unwrap(), 3);
        // 再扫一次看见它 → 恢复正常
        upsert_asset(
            &c,
            &AssetRecord {
                volume_id: "v".into(),
                rel_path: r"a\2.jpg".into(),
                name: "2.jpg".into(),
                kind: "image".into(),
                size: 1,
                ..Default::default()
            },
        )
        .unwrap();
        assert!(missing_assets(&c, "v").unwrap().is_empty());
    }

    #[test]
    fn 从索引移除只删索引() {
        let c = conn("db-forget");
        let id = upsert_asset(
            &c,
            &AssetRecord {
                volume_id: "v".into(),
                rel_path: "x.png".into(),
                name: "x.png".into(),
                kind: "image".into(),
                size: 1,
                ..Default::default()
            },
        )
        .unwrap();
        meta_set(&c, id, "System.Title", "标题").unwrap();
        assert_eq!(forget_assets(&c, &[id]).unwrap(), 1);
        assert_eq!(count_assets(&c).unwrap(), 0);
        assert!(meta_all(&c, id).unwrap().is_empty());
    }

    #[test]
    fn 扩展属性读写() {
        let c = conn("db-meta");
        let id = upsert_asset(
            &c,
            &AssetRecord {
                volume_id: "v".into(),
                rel_path: "v.mp4".into(),
                name: "v.mp4".into(),
                kind: "video".into(),
                size: 1,
                ..Default::default()
            },
        )
        .unwrap();
        meta_set(&c, id, "System.Media.Duration", "12345").unwrap();
        meta_set(&c, id, "System.Video.FrameWidth", "1920").unwrap();
        meta_set(&c, id, "System.Media.Duration", "54321").unwrap();
        let all = meta_all(&c, id).unwrap();
        assert_eq!(all.len(), 2);
        assert!(all.contains(&("System.Media.Duration".to_string(), "54321".to_string())));
    }

    #[test]
    fn 工作根幂等() {
        let c = conn("db-roots");
        let a = upsert_scan_root(&c, r"D:\素材", false, None, 111).unwrap();
        let b = upsert_scan_root(&c, r"D:\素材", true, Some(3), 222).unwrap();
        assert_eq!(a, b);
        let (follow, depth, at): (i64, i64, i64) = c
            .query_row("SELECT follow_links, max_depth, last_scan_at FROM scan_roots", [], |r| {
                Ok((r.get(0)?, r.get(1)?, r.get(2)?))
            })
            .unwrap();
        assert_eq!((follow, depth, at), (1, 3, 222));
    }

    #[test]
    fn 引用映射先删后插() {
        let c = conn("db-refs");
        replace_refs(&c, r"D:\p\manifest.xml", None, &[("bj.png".into(), 3), ("xt.png".into(), 7)])
            .unwrap();
        assert_eq!(refs_for(&c, "bj.png").unwrap(), vec![(r"D:\p\manifest.xml".to_string(), 3)]);
        replace_refs(&c, r"D:\p\manifest.xml", None, &[("bj.png".into(), 9)]).unwrap();
        assert_eq!(refs_for(&c, "xt.png").unwrap().len(), 0);
        assert_eq!(refs_for(&c, "bj.png").unwrap(), vec![(r"D:\p\manifest.xml".to_string(), 9)]);
    }

    #[test]
    fn 库文件全部落在库目录内() {
        let layout = temp_layout("db-inlibrary");
        let c = open_library(&layout).unwrap();
        drop(c);
        assert!(layout.db.is_file(), "库文件应落在库目录");
        assert!(layout.db.starts_with(&layout.root));
        // 素材树里不会出现任何库文件（这里只校验库侧；素材树零写入由扫描用例保证）
        assert!(layout.thumbs.is_dir());
    }
}
