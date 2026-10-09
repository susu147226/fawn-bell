//! `ScanUseCase`（执行版 §7.1 / §15 P0）。
//!
//! 职责：把「核心域的规则」与「基础设施的遍历」拼成一次**只读**扫描，产出
//! ①进度流 ②按类型统计的摘要 ③目录聚合行 ④文件行。
//!
//! P0 的结果**只存内存**（不写 SQLite、不落盘），退出即丢——索引库与增量扫描属 P1（§15）。

use std::path::{Path, PathBuf};
use std::sync::atomic::AtomicBool;
use std::time::{Duration, Instant};

use crate::domain::aggregate::{build_dirs, DirAgg, FileMeta, DirMeta};
use crate::domain::guard::{check_root, exclusion_for, RootVerdict, Sensitive};
use crate::domain::kind::{Kind, ALL};
use crate::infra::walker::{self, WalkItem, WalkOptions};

/// 进度事件的节流间隔（§11 要求扫描期间界面不冻结；节流避免 IPC 洪泛）。
const PROGRESS_INTERVAL: Duration = Duration::from_millis(80);

/// 单次扫描文件数上限默认值（§10）。
pub const DEFAULT_FILE_LIMIT: u64 = 200_000;

#[derive(Debug, Clone)]
pub struct ScanOptions {
    pub scan_id: u64,
    pub follow_links: bool,
    pub max_depth: Option<usize>,
    pub file_limit: u64,
    /// 库目录：若落在素材根之内，必须强制排除（§6.2、§14②）。
    pub library_dir: PathBuf,
    pub sensitive: Sensitive,
}

impl ScanOptions {
    pub fn new(scan_id: u64, library_dir: PathBuf) -> Self {
        let sensitive = crate::infra::paths::sensitive_dirs(library_dir.clone());
        Self {
            scan_id,
            follow_links: false,
            max_depth: None,
            file_limit: DEFAULT_FILE_LIMIT,
            library_dir,
            sensitive,
        }
    }
}

#[derive(Debug, Clone, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ScanProgress {
    pub scan_id: u64,
    pub files: u64,
    pub dirs_done: u64,
    pub dirs_pending: u64,
    pub bytes: u64,
    pub current: String,
    pub elapsed_ms: u64,
    /// 预估剩余毫秒；`None` 表示样本不足尚不可估。
    pub eta_ms: Option<u64>,
    pub phase: &'static str,
}

#[derive(Debug, Clone, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct KindStat {
    pub kind: Kind,
    pub count: u64,
    pub bytes: u64,
}

#[derive(Debug, Clone, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct BigDir {
    pub rel_path: String,
    pub entries: u32,
}

#[derive(Debug, Clone, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ScanSummary {
    pub scan_id: u64,
    /// 用户所选素材根（绝对路径）。
    pub root: String,
    pub dir_count: u64,
    pub file_count: u64,
    pub total_bytes: u64,
    pub elapsed_ms: u64,
    /// 按 [`ALL`] 顺序，含 0 项（§7.1「图片 1,820 · 视频 96 · …」）。
    pub by_kind: Vec<KindStat>,
    pub warnings: Vec<String>,
    /// 达到单次扫描文件数上限而提前结束（§10）。
    pub truncated: bool,
    pub followed_links: bool,
    /// 单目录条目数预警（§10，默认 5,000）。
    pub big_dirs: Vec<BigDir>,
    /// 本次实际强制排除的子树（库目录落在素材根内时出现）。
    pub excluded: Vec<String>,
    /// 库目录（仅展示，P0 不写入）。
    pub library_dir: String,
}

#[derive(Debug, Clone, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ScanResult {
    pub summary: ScanSummary,
    pub dirs: Vec<DirAgg>,
    pub files: Vec<FileMeta>,
}

#[derive(Debug)]
pub enum ScanOutcome {
    Done(Box<ScanResult>),
    /// §7.1：取消只丢弃本次结果，不影响任何已有数据。
    Cancelled { files: u64, elapsed_ms: u64 },
    Failed(String),
}

/// 执行一次只读扫描。`on_progress` 会被节流调用（约每 [`PROGRESS_INTERVAL`] 一次，且收尾必调一次）。
pub fn run_scan(
    root: &Path,
    opts: &ScanOptions,
    cancel: &AtomicBool,
    mut on_progress: impl FnMut(ScanProgress),
) -> ScanOutcome {
    // ① 工作根判定（纯逻辑，理由原样展示给用户）
    match check_root(root, &opts.sensitive) {
        RootVerdict::Allowed => {}
        RootVerdict::Refused(reason) => return ScanOutcome::Failed(reason),
    }

    // ② 库目录落在素材根内 → 强制排除
    let excluded: Vec<PathBuf> = exclusion_for(root, &opts.library_dir).into_iter().collect();
    let walk_opts = WalkOptions {
        follow_links: opts.follow_links,
        max_depth: opts.max_depth,
        excluded: excluded.clone(),
        file_limit: opts.file_limit,
    };

    let started = Instant::now();
    let mut dirs: Vec<DirMeta> = Vec::new();
    let mut files: Vec<FileMeta> = Vec::new();
    let mut dirs_entered: u64 = 0;
    let mut dirs_done: u64 = 0;
    let mut bytes: u64 = 0;
    let mut current = String::new();
    let mut last_emit = Instant::now();

    let outcome = walker::walk(root, &walk_opts, cancel, |item| {
        match item {
            WalkItem::DirEntered { meta, .. } => {
                dirs_entered += 1;
                current = meta.rel_path.clone();
                dirs.push(meta);
            }
            WalkItem::DirFinished { .. } => {
                dirs_done += 1;
            }
            WalkItem::File(f) => {
                bytes += f.size;
                current = f.rel_path.clone();
                files.push(f);
            }
        }

        // 节流上报进度（§11：扫描期间界面不冻结且可取消）
        let now = Instant::now();
        if now.duration_since(last_emit) >= PROGRESS_INTERVAL {
            last_emit = now;
            let elapsed = now.duration_since(started).as_millis() as u64;
            let pending = dirs_entered.saturating_sub(dirs_done);
            // 基于目录队列的预估：用已完成目录的平均耗时推算剩余目录
            let eta_ms = if dirs_done >= 4 && pending > 0 {
                Some(elapsed.saturating_mul(pending) / dirs_done.max(1))
            } else {
                None
            };
            on_progress(ScanProgress {
                scan_id: opts.scan_id,
                files: files.len() as u64,
                dirs_done,
                dirs_pending: pending,
                bytes,
                current: current.clone(),
                elapsed_ms: elapsed,
                eta_ms,
                phase: "正在扫描目录与文件",
            });
        }
    });

    let elapsed_ms = started.elapsed().as_millis() as u64;

    if cancel.load(std::sync::atomic::Ordering::Relaxed) {
        return ScanOutcome::Cancelled {
            files: files.len() as u64,
            elapsed_ms,
        };
    }

    let walked = match outcome {
        Ok(w) => w,
        Err(e) => return ScanOutcome::Failed(format!("无法扫描 `{}`：{e}", root.display())),
    };

    let root_meta = dirs
        .iter()
        .find(|d| d.rel_path.is_empty())
        .cloned()
        .unwrap_or(DirMeta {
            rel_path: String::new(),
            mtime_ms: 0,
            cloud: false,
        });
    let child_dirs: Vec<DirMeta> = dirs
        .iter()
        .filter(|d| !d.rel_path.is_empty())
        .cloned()
        .collect();
    let dir_aggs = build_dirs(&root_meta, &child_dirs, &files);

    let by_kind = ALL
        .iter()
        .map(|k| {
            let mut count = 0u64;
            let mut kb = 0u64;
            for f in &files {
                if f.kind == *k {
                    count += 1;
                    kb += f.size;
                }
            }
            KindStat {
                kind: *k,
                count,
                bytes: kb,
            }
        })
        .collect();

    let summary = ScanSummary {
        scan_id: opts.scan_id,
        root: root.to_string_lossy().to_string(),
        dir_count: walked.dir_count,
        file_count: walked.file_count,
        total_bytes: walked.total_bytes,
        elapsed_ms,
        by_kind,
        warnings: walked.warnings,
        truncated: walked.truncated,
        followed_links: opts.follow_links,
        big_dirs: walked
            .big_dirs
            .into_iter()
            .map(|(rel_path, entries)| BigDir { rel_path, entries })
            .collect(),
        excluded: excluded
            .iter()
            .map(|p| p.to_string_lossy().to_string())
            .collect(),
        library_dir: opts.library_dir.to_string_lossy().to_string(),
    };

    // 收尾进度（保证界面拿到最终计数）
    on_progress(ScanProgress {
        scan_id: opts.scan_id,
        files: summary.file_count,
        dirs_done,
        dirs_pending: 0,
        bytes: summary.total_bytes,
        current,
        elapsed_ms,
        eta_ms: Some(0),
        phase: "扫描完成",
    });

    ScanOutcome::Done(Box::new(ScanResult {
        summary,
        dirs: dir_aggs,
        files,
    }))
}
