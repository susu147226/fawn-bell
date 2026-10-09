//! `IndexUseCase`（执行版 §7.1 扫描与索引 / §13.2 素材索引 / §13.4 增量与失联）。
//!
//! 职责：把**一次已完成的扫描结果**写进库，并按「体积 + 修改时间 + 创建时间」判定增量：
//! - 新建 / 变更的条目：写索引 + 首尾各 64 KB 分段哈希（§7.12 两级指纹第一级）
//!   + 交给 [`Metadata`] 补 EXIF / Shell 属性（§6.4）；
//! - 未变的条目：整条跳过（不读元数据、不重算哈希），只计入 `unchanged`；
//! - 本次没再出现的条目：标记 `missing`，**不删除元数据**（§7.1）；
//! - 顺带刷新引用映射（§7.1：与索引同批完成，不额外遍历素材树）。
//!
//! 只读约定：本用例只读素材文件，全部写入都落在库目录内（§14 第 1 条）。

use std::collections::{HashMap, HashSet};
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};

use crate::domain::aggregate::FileMeta;
use crate::domain::kind::{self, Kind};
use crate::infra::db::{self, AssetRecord, Fingerprint};
use crate::infra::{hash, library, refscan, volume};

/// 索引阶段的进度（与扫描阶段用同一个进度事件，靠 `phase` 区分）。
#[derive(Debug, Clone, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct IndexProgress {
    pub done: u64,
    pub total: u64,
    pub phase: &'static str,
}

/// 一次索引的结果摘要（放进 `scan://done` 与 CLI 输出，界面据此显示「新增 / 更新 / 未变 / 缺失」）。
#[derive(Debug, Clone, Default, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct IndexReport {
    pub volume_id: String,
    /// 库里现有条目总数。
    pub entries: i64,
    pub inserted: u64,
    pub updated: u64,
    pub unchanged: u64,
    /// 本次没再出现、被标记为缺失的条数（§7.1）。
    pub missing: usize,
    /// 上次标成缺失、这次又看见了、恢复正常的条数。
    pub unmissed: usize,
    pub hashed: u64,
    /// 本次重建的引用行数（§6.3.1）。
    pub refs: usize,
    /// 索引中途被取消（已写入的部分都是磁盘真实状态，无害）。
    pub cancelled: bool,
    pub db: String,
    pub warnings: Vec<String>,
}

/// 元数据提供者：图片走 EXIF、视频/音频/文档走 Shell 属性（§6.4）。
///
/// 索引层只决定「什么时候读」（仅新建/变更的条目），不关心「怎么读」。
pub trait Metadata {
    fn fill(&self, abs: &Path, kind: Kind, rec: &mut AssetRecord, extra: &mut Vec<(String, String)>);
}

/// 什么都不读的实现：给测试与「暂不读取元数据」的场景用。
pub struct NoMetadata;

impl Metadata for NoMetadata {
    fn fill(&self, _abs: &Path, _kind: Kind, _rec: &mut AssetRecord, _extra: &mut Vec<(String, String)>) {}
}

/// 每批提交的条目数（事务粒度）。
const BATCH: usize = 400;
/// 每隔多少条上报一次进度。
const PROGRESS_EVERY: u64 = 200;

fn ext_of(name: &str) -> Option<String> {
    let e = kind::ext_of(name);
    if e.is_empty() {
        None
    } else {
        Some(e)
    }
}

/// 索引一次扫描结果。
pub fn index_scan(
    root: &Path,
    files: &[FileMeta],
    layout: &library::LibraryLayout,
    cancel: &AtomicBool,
    provider: &dyn Metadata,
    mut on_progress: impl FnMut(IndexProgress),
) -> Result<IndexReport, String> {
    let mut conn = db::open_library(layout)?;
    let vol = volume::volume_id(root);
    let existing = db::load_volume_index(&conn, &vol)?;
    let existing_lower: HashMap<String, Fingerprint> = existing
        .iter()
        .map(|(k, v)| (k.to_lowercase(), *v))
        .collect();

    let total = files.len() as u64;
    let mut report = IndexReport {
        volume_id: vol.clone(),
        db: layout.db.display().to_string(),
        ..Default::default()
    };
    let mut seen: HashSet<String> = HashSet::with_capacity(files.len());
    let mut done: u64 = 0;

    on_progress(IndexProgress {
        done: 0,
        total,
        phase: "正在建立索引",
    });

    'outer: for chunk in files.chunks(BATCH) {
        if cancel.load(Ordering::Relaxed) {
            report.cancelled = true;
            break;
        }
        let tx = conn.transaction().map_err(|e| format!("打开索引事务失败：{e}"))?;

        for f in chunk {
            done += 1;
            let abs = root.join(f.rel_path.replace('/', "\\"));
            let rel_vol = volume::rel_path_from_volume(&abs);
            let rel_key = rel_vol.to_lowercase();
            seen.insert(rel_key.clone());

            let size = f.size as i64;
            let known = existing_lower.get(&rel_key).copied();
            let unchanged = known
                .map(|p| p.unchanged(size, f.mtime_ms, f.ctime_ms))
                .unwrap_or(false);
            let hashed_before = known.map(|p| p.has_hash).unwrap_or(false);

            // 未变且已算过指纹 → 整条跳过（增量扫描快在这里）
            if unchanged && hashed_before {
                report.unchanged += 1;
                if done % PROGRESS_EVERY == 0 {
                    on_progress(IndexProgress {
                        done,
                        total,
                        phase: "正在建立索引",
                    });
                }
                continue;
            }

            let mut rec = AssetRecord {
                volume_id: vol.clone(),
                rel_path: rel_vol.clone(),
                name: f.name.clone(),
                ext: ext_of(&f.name),
                kind: f.kind.as_str().to_string(),
                size,
                mtime: f.mtime_ms,
                ctime: f.ctime_ms,
                ..Default::default()
            };
            let mut extra: Vec<(String, String)> = Vec::new();

            if !(unchanged && hashed_before) {
                match hash::partial_hash(&abs) {
                    Ok(h) => {
                        rec.hash_partial = Some(h);
                        report.hashed += 1;
                    }
                    Err(e) => report.warnings.push(format!(
                        "无法计算内容指纹 {}：{e}",
                        abs.to_string_lossy()
                    )),
                }
            }

            // 只有新建/变更的条目才读元数据（省掉重复 IO，§7.1 增量）
            if !unchanged {
                provider.fill(&abs, f.kind, &mut rec, &mut extra);
            }

            match db::upsert_asset(&tx, &rec) {
                Ok(id) => {
                    if known.is_some() {
                        report.updated += 1;
                    } else {
                        report.inserted += 1;
                    }
                    for (k, v) in &extra {
                        if let Err(e) = db::meta_set(&tx, id, k, v) {
                            report.warnings.push(format!("写扩展属性失败：{e}"));
                        }
                    }
                    // 引用映射（§6.3.1）：持有者文件才解析
                    if let Some(ext) = rec.ext.as_deref() {
                        if refscan::is_holder(ext) {
                            let need = !unchanged
                                || matches!(db::refs_count_for_holder(&tx, &abs.to_string_lossy()), Ok(0));
                            if need {
                                match read_holder(&abs) {
                                    Ok(text) => {
                                        let refs = refscan::extract_refs(&text);
                                        match db::replace_refs(&tx, &abs.to_string_lossy(), Some(id), &refs) {
                                            Ok(n) => report.refs += n,
                                            Err(e) => report.warnings.push(format!("写引用映射失败：{e}")),
                                        }
                                    }
                                    Err(e) => report
                                        .warnings
                                        .push(format!("无法读取引用持有者 {}：{e}", abs.to_string_lossy())),
                                }
                            }
                        }
                    }
                }
                Err(e) => report.warnings.push(format!("写索引失败 {}：{e}", rec.rel_path)),
            }

            if done % PROGRESS_EVERY == 0 {
                on_progress(IndexProgress {
                    done,
                    total,
                    phase: "正在建立索引",
                });
            }
            if cancel.load(Ordering::Relaxed) {
                report.cancelled = true;
                break 'outer;
            }
        }

        tx.commit().map_err(|e| format!("提交索引事务失败：{e}"))?;
    }

    if !report.cancelled {
        report.missing = db::mark_missing(&conn, &vol, &seen)?;
        // 反过来也要做：未变条目被整条跳过，它们身上残留的 missing 必须在这里清掉，
        // 否则「文件明明在、索引却说是缺失」会让去重/统计全部失真。
        report.unmissed = db::clear_missing(&conn, &vol, &seen)?;
        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_millis() as i64)
            .unwrap_or(0);
        let _ = db::upsert_scan_root(&conn, &root.to_string_lossy(), false, None, now);
    }
    report.entries = db::count_assets(&conn)?;

    on_progress(IndexProgress {
        done,
        total,
        phase: "索引完成",
    });
    Ok(report)
}

/// 读一个引用持有者文件（带体积上限，超过就只读前 [`refscan::MAX_HOLDER_BYTES`]）。
fn read_holder(abs: &Path) -> std::io::Result<String> {
    use std::io::Read;
    let mut file = std::fs::File::open(abs)?;
    let meta = file.metadata()?;
    if meta.len() > refscan::MAX_HOLDER_BYTES {
        let mut buf = vec![0u8; refscan::MAX_HOLDER_BYTES as usize];
        file.read_exact(&mut buf)?;
        return Ok(String::from_utf8_lossy(&buf).to_string());
    }
    let mut text = String::new();
    file.read_to_string(&mut text)?;
    Ok(text)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::infra::library::LibraryLayout;
    use std::fs;
    use std::path::PathBuf;

    /// 一个可写的场景目录（库 + 素材树都在 crate 的 target 下，绝不碰真实素材）。
    struct Scene {
        root: PathBuf,
        _base: PathBuf,
    }

    fn scene(name: &str) -> (Scene, LibraryLayout) {
        let base = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("target")
            .join("luling-tests")
            .join("index")
            .join(name);
        let _ = fs::remove_dir_all(&base);
        let root = base.join("素材");
        fs::create_dir_all(&root).expect("建素材目录");
        let layout = LibraryLayout::for_root(base.join("库"));
        (Scene { root, _base: base }, layout)
    }

    fn file(name: &str, size: usize) -> FileMeta {
        FileMeta {
            rel_path: name.to_string(),
            name: name.rsplit('/').next().unwrap().to_string(),
            kind: Kind::of_ext(&kind::ext_of(name)),
            size: size as u64,
            mtime_ms: 1_700_000_000_000,
            ctime_ms: 1_699_000_000_000,
            cloud: false,
        }
    }

    fn scan_all(root: &Path) -> Vec<FileMeta> {
        let cancel = AtomicBool::new(false);
        let mut files = Vec::new();
        crate::infra::walker::walk(root, &Default::default(), &cancel, |item| {
            if let crate::infra::walker::WalkItem::File(f) = item {
                files.push(f);
            }
        })
        .expect("遍历");
        files
    }

    #[test]
    fn 首次索引写入条目与卷标识() {
        let (s, layout) = scene("first");
        fs::write(s.root.join("a.png"), b"0123456789").unwrap();
        let files = scan_all(&s.root);
        let cancel = AtomicBool::new(false);
        let rep = index_scan(&s.root, &files, &layout, &cancel, &NoMetadata, |_| {}).unwrap();
        assert_eq!(rep.inserted, 1);
        assert_eq!(rep.entries, 1);
        assert_eq!(rep.missing, 0);
        assert!(rep.volume_id.starts_with("vol:") || rep.volume_id.starts_with("mnt:"));
        assert_eq!(rep.hashed, 1);

        let conn = db::open_library(&layout).unwrap();
        let (kind_col, hp): (String, Option<String>) =
            conn.query_row("SELECT kind, hash_partial FROM assets", [], |r| {
                Ok((r.get(0)?, r.get(1)?))
            })
            .unwrap();
        assert_eq!(kind_col, "image");
        assert!(hp.unwrap_or_default().starts_with("b3p1:"));
    }

    #[test]
    fn 二次索引未变条目全部跳过() {
        let (s, layout) = scene("incremental");
        fs::write(s.root.join("a.png"), b"0123456789").unwrap();
        let cancel = AtomicBool::new(false);
        let files = scan_all(&s.root);
        index_scan(&s.root, &files, &layout, &cancel, &NoMetadata, |_| {}).unwrap();

        let files2 = scan_all(&s.root);
        assert_eq!(files2[0].size, files[0].size, "同一文件体积应一致");
        let rep2 = index_scan(&s.root, &files2, &layout, &cancel, &NoMetadata, |_| {}).unwrap();
        assert_eq!(rep2.unchanged, 1, "体积/修改时间/创建时间都没变 → 整条跳过");
        assert_eq!(rep2.inserted, 0);
        assert_eq!(rep2.updated, 0);
        assert_eq!(rep2.hashed, 0, "未变条目不应重算指纹");
        assert_eq!(rep2.entries, 1);
    }

    #[test]
    fn 文件变大视为变更() {
        let (s, layout) = scene("changed");
        fs::write(s.root.join("a.png"), b"01").unwrap();
        let cancel = AtomicBool::new(false);
        let files = scan_all(&s.root);
        index_scan(&s.root, &files, &layout, &cancel, &NoMetadata, |_| {}).unwrap();

        std::thread::sleep(std::time::Duration::from_millis(20));
        fs::write(s.root.join("a.png"), b"0123456789").unwrap();
        let files2 = scan_all(&s.root);
        let rep2 = index_scan(&s.root, &files2, &layout, &cancel, &NoMetadata, |_| {}).unwrap();
        assert_eq!(rep2.updated, 1);
        assert_eq!(rep2.inserted, 0);
        assert_eq!(rep2.entries, 1);
    }

    #[test]
    fn 消失的文件被标记缺失但元数据保留() {
        let (s, layout) = scene("missing");
        fs::write(s.root.join("keep.png"), b"aaaa").unwrap();
        fs::write(s.root.join("gone.png"), b"bbbb").unwrap();
        let cancel = AtomicBool::new(false);
        let files = scan_all(&s.root);
        index_scan(&s.root, &files, &layout, &cancel, &NoMetadata, |_| {}).unwrap();

        fs::remove_file(s.root.join("gone.png")).unwrap();
        let files2 = scan_all(&s.root);
        let rep2 = index_scan(&s.root, &files2, &layout, &cancel, &NoMetadata, |_| {}).unwrap();
        assert_eq!(rep2.missing, 1);
        assert_eq!(rep2.entries, 2, "缺失条目仍在库里（§7.1 不自动清理）");

        let conn = db::open_library(&layout).unwrap();
        let miss = db::missing_assets(&conn, &rep2.volume_id).unwrap();
        assert_eq!(miss.len(), 1);
        assert!(miss[0].1.ends_with("gone.png"));
    }

    #[test]
    fn 取消时不标记缺失() {
        let (s, layout) = scene("cancel");
        fs::write(s.root.join("a.png"), b"aaaa").unwrap();
        let cancel = AtomicBool::new(false);
        let files = scan_all(&s.root);
        index_scan(&s.root, &files, &layout, &cancel, &NoMetadata, |_| {}).unwrap();

        let cancel2 = AtomicBool::new(true);
        let rep2 = index_scan(&s.root, &files, &layout, &cancel2, &NoMetadata, |_| {}).unwrap();
        assert!(rep2.cancelled);
        assert_eq!(rep2.missing, 0);
    }

    #[test]
    fn 缺失条目重新出现时恢复且仍算未变() {
        let (s, layout) = scene("unmissed");
        fs::write(s.root.join("a.png"), b"aaaa").unwrap();
        let cancel = AtomicBool::new(false);
        let files = scan_all(&s.root);
        index_scan(&s.root, &files, &layout, &cancel, &NoMetadata, |_| {}).unwrap();

        // 模拟「先扫了别的根，把它标成缺失」
        let vol = volume::volume_id(&s.root);
        {
            let conn = db::open_library(&layout).unwrap();
            conn.execute("UPDATE assets SET missing = 1", []).unwrap();
            assert_eq!(db::missing_assets(&conn, &vol).unwrap().len(), 1);
        }

        // 再扫本根：条目未变（体积/时间都没动）→ 仍走「跳过」路径，但 missing 必须被清掉
        let rep = index_scan(&s.root, &files, &layout, &cancel, &NoMetadata, |_| {}).unwrap();
        assert_eq!(rep.unchanged, 1);
        assert_eq!(rep.unmissed, 1, "重新看见的条目要恢复");
        assert_eq!(rep.missing, 0);

        let conn = db::open_library(&layout).unwrap();
        assert!(db::missing_assets(&conn, &vol).unwrap().is_empty());
    }

    #[test]
    fn 元数据提供者只对被判定为变更的条目调用() {
        struct Counting(std::cell::Cell<u32>);
        impl Metadata for Counting {
            fn fill(&self, _a: &Path, _k: Kind, rec: &mut AssetRecord, extra: &mut Vec<(String, String)>) {
                self.0.set(self.0.get() + 1);
                rec.width = Some(1920);
                rec.height = Some(1080);
                extra.push(("System.Title".into(), "标题".into()));
            }
        }
        let (s, layout) = scene("provider");
        fs::write(s.root.join("a.png"), b"aaaa").unwrap();
        let cancel = AtomicBool::new(false);
        let provider = Counting(std::cell::Cell::new(0));
        let files = scan_all(&s.root);
        index_scan(&s.root, &files, &layout, &cancel, &provider, |_| {}).unwrap();
        index_scan(&s.root, &files, &layout, &cancel, &provider, |_| {}).unwrap();
        assert_eq!(provider.0.get(), 1, "第二次扫描未变 → 不应再读元数据");

        let conn = db::open_library(&layout).unwrap();
        let (w, h): (i64, i64) = conn
            .query_row("SELECT width, height FROM assets", [], |r| Ok((r.get(0)?, r.get(1)?)))
            .unwrap();
        assert_eq!((w, h), (1920, 1080));
        let id: i64 = conn.query_row("SELECT id FROM assets", [], |r| r.get(0)).unwrap();
        assert_eq!(db::meta_all(&conn, id).unwrap().len(), 1);
    }

    #[test]
    fn 引用映射随索引建立() {
        let (s, layout) = scene("refs");
        fs::write(s.root.join("bj.png"), b"aaaa").unwrap();
        fs::write(s.root.join("manifest.xml"), b"<x><img src=\"bj.png\"/></x>").unwrap();
        let cancel = AtomicBool::new(false);
        let files = scan_all(&s.root);
        let rep = index_scan(&s.root, &files, &layout, &cancel, &NoMetadata, |_| {}).unwrap();
        assert!(rep.refs >= 1);

        let conn = db::open_library(&layout).unwrap();
        let holders = db::refs_for(&conn, "bj.png").unwrap();
        assert_eq!(holders.len(), 1);
        assert!(holders[0].0.ends_with("manifest.xml"));
    }
}
