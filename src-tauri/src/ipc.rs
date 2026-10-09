//! UI 层 · Tauri 命令与事件（执行版 §12.2 UI 层 / §15 P0）。
//!
//! 设计要点：
//! - 扫描结果**不随事件传大 payload**：`scan://done` 只给摘要与状态，明细由 `scan_result(scanId)` 按需取。
//! - 同时只允许一个扫描任务；P0 只保留最近一次结果（索引库属 P1）。
//! - 错误一律以中文可读文案返回，界面直接展示，不出现英文堆栈。

use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

use tauri::{AppHandle, Emitter, State};

use crate::app::scan::{run_scan, ScanOptions, ScanOutcome, ScanProgress, ScanResult, ScanSummary};
use crate::infra::paths;

/// 运行槽位。
#[derive(Default)]
struct Inner {
    next_id: u64,
    running: Option<u64>,
    cancel: Option<Arc<AtomicBool>>,
    progress: Option<ScanProgress>,
    last: Option<(u64, Arc<ScanResult>)>,
}

/// P0 的扫描状态只存内存（§15：SQLite 索引属 P1）。
#[derive(Default)]
pub struct ScanState(Arc<Mutex<Inner>>);

impl ScanState {
    pub fn new() -> Self {
        Self::default()
    }

    fn lock(&self) -> Result<std::sync::MutexGuard<'_, Inner>, String> {
        self.0
            .lock()
            .map_err(|_| "扫描状态已损坏，请重启鹿铃。".to_string())
    }
}

#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct AppInfo {
    pub name: &'static str,
    pub version: &'static str,
    pub identifier: &'static str,
    pub author: &'static str,
    pub copyright: &'static str,
    pub library_dir: String,
}

/// 关于信息（§5：「关于」界面须显示版权、产品名、包标识与许可证名）。
#[tauri::command]
pub fn app_info() -> AppInfo {
    AppInfo {
        name: "鹿铃",
        version: env!("CARGO_PKG_VERSION"),
        identifier: "com.yunshumianmian.luling",
        author: "云舒眠眠",
        copyright: "© 2026 云舒眠眠",
        library_dir: paths::library_dir().to_string_lossy().to_string(),
    }
}

#[derive(Clone, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ScanDone {
    pub scan_id: u64,
    /// `done` / `cancelled` / `failed`。
    pub status: &'static str,
    pub summary: Option<ScanSummary>,
    pub message: Option<String>,
    /// P1：本次扫描写入索引库的结果（新增 / 更新 / 未变 / 缺失 / 指纹 / 引用）。
    pub index: Option<crate::app::index::IndexReport>,
    /// 建立索引失败的原因（扫描本身已成功，索引失败不把整次扫描算失败）。
    pub index_error: Option<String>,
}

#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ScanSnapshot {
    pub running: Option<u64>,
    pub progress: Option<ScanProgress>,
    pub has_result: bool,
}

/// 开始一次只读扫描，立即返回 scanId；进度走 `scan://progress`，结束走 `scan://done`。
#[tauri::command]
pub fn scan_start(
    app: AppHandle,
    state: State<'_, ScanState>,
    root: String,
    follow_links: Option<bool>,
) -> Result<u64, String> {
    let inner = state.0.clone();
    let (scan_id, cancel) = {
        let mut g = state.lock()?;
        if let Some(id) = g.running {
            return Err(format!(
                "已有一个扫描任务在进行中（#{id}）。请先取消它或等它结束。"
            ));
        }
        g.next_id += 1;
        let id = g.next_id;
        let cancel = Arc::new(AtomicBool::new(false));
        g.running = Some(id);
        g.cancel = Some(cancel.clone());
        g.progress = None;
        (id, cancel)
    };

    let root_path = PathBuf::from(&root);
    let mut opts = ScanOptions::new(scan_id, paths::library_dir());
    opts.follow_links = follow_links.unwrap_or(false);

    let app_handle = app.clone();
    let inner_thread = inner.clone();
    tauri::async_runtime::spawn(async move {
        let done = {
            let outcome = run_scan(&root_path, &opts, &cancel, |p| {
                if let Ok(mut g) = inner_thread.lock() {
                    g.progress = Some(p.clone());
                }
                let _ = app_handle.emit("scan://progress", &p);
            });

            match outcome {
                ScanOutcome::Done(result) => {
                    let summary = result.summary.clone();

                    // §7.1：扫描成功后把结果写进索引库（增量判定 + 首尾 64 KB 指纹 + 元数据 + 引用映射）。
                    // 取消 / 失败的扫描不会走到这里，所以「取消只丢弃本次结果」依然成立。
                    let emit_handle = app_handle.clone();
                    let emit_bytes = summary.total_bytes;
                    let emit_elapsed = summary.elapsed_ms;
                    let on_index_progress = move |p: crate::app::index::IndexProgress| {
                        let _ = emit_handle.emit(
                            "scan://progress",
                            &serde_json::json!({
                                "scanId": scan_id,
                                "files": p.done,
                                "dirsDone": p.done,
                                "dirsPending": p.total.saturating_sub(p.done),
                                "bytes": emit_bytes,
                                "current": "",
                                "elapsedMs": emit_elapsed,
                                "etaMs": null,
                                "phase": p.phase,
                            }),
                        );
                    };
                    let (index, index_error) = match crate::app::index::index_scan(
                        &PathBuf::from(&summary.root),
                        &result.files,
                        &crate::infra::library::layout(),
                        &cancel,
                        &crate::app::metadata::FullMetadata,
                        on_index_progress,
                    ) {
                        Ok(r) => (Some(r), None),
                        Err(e) => (None, Some(format!("建立索引失败：{e}"))),
                    };

                    if let Ok(mut g) = inner_thread.lock() {
                        g.last = Some((scan_id, Arc::new(*result)));
                        g.running = None;
                        g.cancel = None;
                        g.progress = None;
                    }
                    ScanDone {
                        scan_id,
                        status: "done",
                        summary: Some(summary),
                        message: None,
                        index,
                        index_error,
                    }
                }
                ScanOutcome::Cancelled { files, elapsed_ms } => {
                    if let Ok(mut g) = inner_thread.lock() {
                        g.running = None;
                        g.cancel = None;
                        g.progress = None;
                    }
                    ScanDone {
                        scan_id,
                        status: "cancelled",
                        summary: None,
                        message: Some(format!(
                            "已取消（取消前已浏览 {files} 个文件，用时 {elapsed_ms} ms）。本次结果已丢弃，磁盘上没有任何改动。"
                        )),
                        index: None,
                        index_error: None,
                    }
                }
                ScanOutcome::Failed(msg) => {
                    if let Ok(mut g) = inner_thread.lock() {
                        g.running = None;
                        g.cancel = None;
                        g.progress = None;
                    }
                    ScanDone {
                        scan_id,
                        status: "failed",
                        summary: None,
                        message: Some(msg),
                        index: None,
                        index_error: None,
                    }
                }
            }
        };
        let _ = app_handle.emit("scan://done", &done);
    });

    Ok(scan_id)
}

/// 请求取消当前扫描；返回是否确实有任务被取消。
#[tauri::command]
pub fn scan_cancel(state: State<'_, ScanState>) -> Result<bool, String> {
    let g = state.lock()?;
    match &g.cancel {
        Some(c) => {
            c.store(true, Ordering::Relaxed);
            Ok(true)
        }
        None => Ok(false),
    }
}

/// 取某次扫描的明细（文件夹聚合行 + 文件行）。
#[tauri::command]
pub fn scan_result(state: State<'_, ScanState>, scan_id: u64) -> Result<ScanResult, String> {
    let g = state.lock()?;
    match &g.last {
        Some((id, r)) if *id == scan_id => Ok((**r).clone()),
        Some((id, _)) => Err(format!(
            "扫描结果 #{scan_id} 已被后来的扫描 #{id} 替换，请重新扫描该文件夹。"
        )),
        None => Err("还没有可用的扫描结果，请先选择素材文件夹并扫描。".to_string()),
    }
}

#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct MetaProp {
    pub key: String,
    pub value: String,
}

/// 索引里某个素材的元数据（§6.4 EXIF/Shell、§8.5 右栏详情）。
#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct AssetMetaDto {
    pub width: Option<i64>,
    pub height: Option<i64>,
    pub capture_time: Option<i64>,
    pub camera: Option<String>,
    pub gps_lat: Option<f64>,
    pub gps_lon: Option<f64>,
    pub orientation: Option<i64>,
    /// 索引后被外部删除/移动（§7.1）。
    pub missing: bool,
    /// Shell 扩展属性（时长、码率、艺术家、标题、作者…）。
    pub props: Vec<MetaProp>,
}

/// 缩略图（PNG 的 data URL）。
///
/// §12.3：优先走系统缩略图接口；拿不到返回 `null`，界面**降级为类型图标**而不是编造图形。
/// §10：缓存写在库目录内，命中即返回；超上限由 [`crate::infra::thumb::evict`] 淘汰。
#[tauri::command]
pub fn thumb_data_url(path: String) -> Result<Option<String>, String> {
    let p = PathBuf::from(&path);
    if !p.is_file() {
        return Ok(None);
    }
    let layout = crate::infra::library::layout();
    match crate::infra::thumb::bytes_for(&layout, &p) {
        Some(bytes) => Ok(Some(format!(
            "data:image/png;base64,{}",
            crate::infra::png::base64(&bytes)
        ))),
        None => Ok(None),
    }
}

/// 取某素材在索引里的元数据。
///
/// 界面给的是「素材根 + 根内相对路径」，索引的主键是「卷标识 + 相对卷根路径」（§13.4），
/// 这里做一次换算，界面不需要知道索引的存储口径。
#[tauri::command]
pub fn asset_meta(root: String, rel_path: String) -> Result<Option<AssetMetaDto>, String> {
    let abs = PathBuf::from(&root).join(rel_path.replace('/', "\\"));
    let vol = crate::infra::volume::volume_id(std::path::Path::new(&root));
    let rel = crate::infra::volume::rel_path_from_volume(&abs);
    let conn = crate::infra::db::open_library(&crate::infra::library::layout())?;
    let Some(detail) = crate::infra::db::asset_detail(&conn, &vol, &rel)? else {
        return Ok(None);
    };
    let props = crate::infra::db::meta_all(&conn, detail.id)?
        .into_iter()
        .map(|(key, value)| MetaProp { key, value })
        .collect();
    Ok(Some(AssetMetaDto {
        width: detail.width,
        height: detail.height,
        capture_time: detail.capture_time,
        camera: detail.camera,
        gps_lat: detail.gps_lat,
        gps_lon: detail.gps_lon,
        orientation: detail.orientation,
        missing: detail.missing,
        props,
    }))
}

/// 界面重载后恢复进度显示（P0 用不到也可安全调用）。
#[tauri::command]
pub fn scan_snapshot(state: State<'_, ScanState>) -> Result<ScanSnapshot, String> {
    let g = state.lock()?;
    Ok(ScanSnapshot {
        running: g.running,
        progress: g.progress.clone(),
        has_result: g.last.is_some(),
    })
}
