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

    // §10：缩略图缓存超上限时按 LRU 淘汰。逐次统计缓存目录代价太高，按调用次数抽样触发即可。
    static THUMB_CALLS: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
    if THUMB_CALLS.fetch_add(1, Ordering::Relaxed) % 256 == 255 {
        let _ = crate::infra::thumb::evict(&layout);
    }

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

/// 库位置信息（设置页 / 重定位向导要用；§13.1 / §13.4）。
#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct LibraryInfoDto {
    pub root: String,
    pub db: String,
    pub thumbs: String,
    pub backups: String,
    /// `appData` / `portable`。
    pub location: &'static str,
    /// 便携位置不可写而降级时的说明（没有降级则为 null）。
    pub degraded: Option<String>,
}

#[tauri::command]
pub fn library_info() -> LibraryInfoDto {
    let res = crate::infra::library::cached();
    LibraryInfoDto {
        root: res.layout.root.to_string_lossy().to_string(),
        db: res.layout.db.to_string_lossy().to_string(),
        thumbs: res.layout.thumbs.to_string_lossy().to_string(),
        backups: res.layout.backups.to_string_lossy().to_string(),
        location: res.location.as_str(),
        degraded: res.degraded.clone(),
    }
}

/// 内容去重报告（§7.12）：判定与「重复内容」集合同源，界面入口直接展示它。
#[tauri::command]
pub fn dedupe_report(policy: Option<String>) -> Result<crate::app::dedupe::DedupeReport, String> {
    let keep = policy
        .as_deref()
        .and_then(crate::domain::dedupe::KeepPolicy::parse)
        .unwrap_or_default();
    let cancel = AtomicBool::new(false);
    crate::app::dedupe::run(&crate::infra::library::layout(), keep, true, &cancel)
}

/// 重新定位素材树：只出计划，不改写任何索引（§13.4）。
#[tauri::command]
pub fn relocate_plan(
    old_root: String,
    new_root: String,
) -> Result<crate::app::library::RelocatePlan, String> {
    let (plan, _) = crate::app::library::run(
        &crate::infra::library::layout(),
        std::path::Path::new(&old_root),
        std::path::Path::new(&new_root),
        false,
        false,
    )?;
    Ok(plan)
}

/// 应用重定位：高置信一档一律改写；「待确认」需显式放开。返回改写条数。
#[tauri::command]
pub fn relocate_apply(
    old_root: String,
    new_root: String,
    include_confirm: bool,
) -> Result<usize, String> {
    let (_, changed) = crate::app::library::run(
        &crate::infra::library::layout(),
        std::path::Path::new(&old_root),
        std::path::Path::new(&new_root),
        true,
        include_confirm,
    )?;
    Ok(changed)
}

/* ── 虚拟变更集（§7.2） ───────────────────────────────────────────── */

/// 草稿列表 + 计数（界面状态条用「待提交 M 项，其中 K 项有问题」）。
#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DraftListDto {
    pub drafts: Vec<crate::domain::drafts::Draft>,
    pub count: usize,
    pub problems: usize,
    /// 重做栈里还有几条（状态条按钮启用条件）。
    pub redo: usize,
}

fn draft_dto(state: &crate::app::drafts::DraftState) -> Result<DraftListDto, String> {
    Ok(DraftListDto {
        drafts: state.snapshot()?,
        count: state.len()?,
        problems: state.problems()?,
        redo: state.redo_len()?,
    })
}

#[tauri::command]
pub fn draft_list(
    app: AppHandle,
    state: State<'_, crate::app::drafts::DraftState>,
) -> Result<DraftListDto, String> {
    let conn = crate::infra::db::open_library(&crate::infra::library::layout())?;
    state.ensure_loaded(&conn)?;
    let dto = draft_dto(&state)?;
    let _ = app.emit("draft://changed", &dto);
    Ok(dto)
}

/// 追加一条草稿。预检结论**不阻断加入**（§7.2：加入即标注，用户在行内看到角标与原因）。
#[tauri::command]
pub fn draft_add(
    app: AppHandle,
    state: State<'_, crate::app::drafts::DraftState>,
    op: String,
    src: String,
    dst: Option<String>,
    asset_id: Option<i64>,
) -> Result<DraftListDto, String> {
    let conn = crate::infra::db::open_library(&crate::infra::library::layout())?;
    state.ensure_loaded(&conn)?;
    let op = crate::domain::drafts::DraftOp::parse(&op).ok_or_else(|| format!("未知操作 `{op}`"))?;
    let mut draft = crate::domain::drafts::Draft::new(op, src, dst);
    draft.asset_id = asset_id;
    state.add(&conn, draft)?;
    let dto = draft_dto(&state)?;
    let _ = app.emit("draft://changed", &dto);
    Ok(dto)
}

#[tauri::command]
pub fn draft_undo(
    app: AppHandle,
    state: State<'_, crate::app::drafts::DraftState>,
) -> Result<DraftListDto, String> {
    let conn = crate::infra::db::open_library(&crate::infra::library::layout())?;
    state.ensure_loaded(&conn)?;
    state.undo(&conn)?;
    let dto = draft_dto(&state)?;
    let _ = app.emit("draft://changed", &dto);
    Ok(dto)
}

#[tauri::command]
pub fn draft_redo(
    app: AppHandle,
    state: State<'_, crate::app::drafts::DraftState>,
) -> Result<DraftListDto, String> {
    let conn = crate::infra::db::open_library(&crate::infra::library::layout())?;
    state.ensure_loaded(&conn)?;
    state.redo(&conn)?;
    let dto = draft_dto(&state)?;
    let _ = app.emit("draft://changed", &dto);
    Ok(dto)
}

/// 放弃全部变更（§7.2「放弃变更并关闭」；界面需二次确认，这里只负责清空）。
#[tauri::command]
pub fn draft_clear(
    app: AppHandle,
    state: State<'_, crate::app::drafts::DraftState>,
) -> Result<DraftListDto, String> {
    let conn = crate::infra::db::open_library(&crate::infra::library::layout())?;
    state.ensure_loaded(&conn)?;
    state.clear(&conn)?;
    let dto = draft_dto(&state)?;
    let _ = app.emit("draft://changed", &dto);
    Ok(dto)
}

/// 投影：把一批真实路径折算成界面该显示的样子（§7.2 投影视图）。
#[tauri::command]
pub fn draft_project(
    state: State<'_, crate::app::drafts::DraftState>,
    paths: Vec<String>,
) -> Result<Vec<crate::domain::drafts::Projection>, String> {
    let conn = crate::infra::db::open_library(&crate::infra::library::layout())?;
    state.ensure_loaded(&conn)?;
    state.project(&paths)
}

/* ── 命名预设（§7.3.2） ───────────────────────────────────────────── */

/// 预设套用结果：模板 + **立刻**给出的前三项预览（§7.3.2 固定契约）。
#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PresetApplyDto {
    pub template: String,
    pub preview: Vec<String>,
}

#[tauri::command]
pub fn preset_list() -> Result<Vec<crate::app::naming::PresetDto>, String> {
    let conn = crate::infra::db::open_library(&crate::infra::library::layout())?;
    crate::app::naming::list(&conn)
}

#[tauri::command]
pub fn preset_save(
    id: Option<i64>,
    base_name: String,
    label: Option<String>,
    template: String,
) -> Result<i64, String> {
    let conn = crate::infra::db::open_library(&crate::infra::library::layout())?;
    crate::app::naming::save(&conn, id, &base_name, label.as_deref(), &template)
}

#[tauri::command]
pub fn preset_delete(id: i64) -> Result<(), String> {
    let conn = crate::infra::db::open_library(&crate::infra::library::layout())?;
    crate::app::naming::remove(&conn, id)
}

#[tauri::command]
pub fn preset_reorder(ids: Vec<i64>) -> Result<(), String> {
    let conn = crate::infra::db::open_library(&crate::infra::library::layout())?;
    crate::app::naming::reorder(&conn, &ids)
}

#[tauri::command]
pub fn preset_export() -> Result<String, String> {
    let conn = crate::infra::db::open_library(&crate::infra::library::layout())?;
    crate::app::naming::export_json(&conn)
}

#[tauri::command]
pub fn preset_import(json: String) -> Result<usize, String> {
    let conn = crate::infra::db::open_library(&crate::infra::library::layout())?;
    crate::app::naming::import_json(&conn, &json)
}

#[tauri::command]
pub fn preset_apply(
    id: i64,
    ext: String,
    start: u64,
    rule: crate::domain::naming::SeqRule,
) -> Result<PresetApplyDto, String> {
    let conn = crate::infra::db::open_library(&crate::infra::library::layout())?;
    let (template, preview) = crate::app::naming::apply(&conn, id, &ext, start, &rule)?;
    Ok(PresetApplyDto { template, preview })
}

/// 模板实时预览（§7.3.3 第 2 点：起始值 / 补零位数 / 作用域任一变动都要立刻出结果）。
#[tauri::command]
pub fn naming_preview(
    template: String,
    rule: crate::domain::naming::SeqRule,
    stem: String,
    ext: String,
) -> Result<Vec<crate::domain::naming::Rendered>, String> {
    let mut out = Vec::new();
    for i in 0..3u64 {
        let ctx = crate::domain::naming::NameCtx {
            stem: &stem,
            ext: &ext,
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
        out.push(crate::domain::naming::render(&template, &ctx, &rule));
    }
    Ok(out)
}

/* ── 分组与保护区（§7.5 / §7.6） ─────────────────────────────────── */

/// 保护区的统计（§7.6：总数 + 当周新增，两个数字并列显示）。
#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ProtectionStatsDto {
    pub total: i64,
    pub week_new: i64,
}

#[tauri::command]
pub fn groups_list() -> Result<Vec<crate::infra::db::GroupRow>, String> {
    let conn = crate::infra::db::open_library(&crate::infra::library::layout())?;
    crate::app::groups::list(&conn)
}

#[tauri::command]
pub fn group_create(name: String) -> Result<i64, String> {
    let conn = crate::infra::db::open_library(&crate::infra::library::layout())?;
    crate::app::groups::create(&conn, &name)
}

#[tauri::command]
pub fn group_rename(id: i64, name: String) -> Result<(), String> {
    let conn = crate::infra::db::open_library(&crate::infra::library::layout())?;
    crate::app::groups::rename(&conn, id, &name)
}

/// 删除分组：只删分组与成员关系，保护区 / 标签 / 已整理标记都不动（§7.6）。
#[tauri::command]
pub fn group_delete(id: i64) -> Result<(), String> {
    let conn = crate::infra::db::open_library(&crate::infra::library::layout())?;
    crate::app::groups::remove(&conn, id)
}

#[tauri::command]
pub fn group_add_members(id: i64, asset_ids: Vec<i64>) -> Result<usize, String> {
    let conn = crate::infra::db::open_library(&crate::infra::library::layout())?;
    crate::app::groups::add_members(&conn, id, &asset_ids)
}

#[tauri::command]
pub fn group_remove_members(id: i64, asset_ids: Vec<i64>) -> Result<usize, String> {
    let conn = crate::infra::db::open_library(&crate::infra::library::layout())?;
    crate::app::groups::remove_members(&conn, id, &asset_ids)
}

/// 求值一个集合的成员（§7.5：智能集合每次打开动态求值）。
///
/// `重复内容` 读的是**内容去重用例物化进 `asset_group` 的那份成员**，与去重判定同源（§16⑰①）。
#[tauri::command]
pub fn group_members_eval(id: i64) -> Result<Vec<i64>, String> {
    let conn = crate::infra::db::open_library(&crate::infra::library::layout())?;
    let groups = crate::app::groups::list(&conn)?;
    let g = groups
        .into_iter()
        .find(|x| x.id == id)
        .ok_or_else(|| "集合不存在".to_string())?;
    if g.kind != "smart" {
        return crate::infra::db::group_members(&conn, id);
    }
    let rule = g.rule_json.clone().unwrap_or_default();
    // 「重复内容」的成员由内容去重用例物化在 asset_group 里（同源）；规则串里没有可识别 kind 的
    // 历史集合（P1 期间的 `ensure_smart_group` 建过一条）也走物化成员，绝不在这里重算第二套判定。
    let known_kind = serde_json::from_str::<serde_json::Value>(&rule)
        .ok()
        .and_then(|v| v.get("kind").and_then(|k| k.as_str()).map(|s| s.to_string()));
    if rule.contains("duplicates") || known_kind.is_none() {
        return crate::infra::db::group_members(&conn, id);
    }
    crate::app::groups::eval_smart(&conn, &rule, &[])
}

#[tauri::command]
pub fn protected_list() -> Result<Vec<crate::infra::db::ProtectionRow>, String> {
    let conn = crate::infra::db::open_library(&crate::infra::library::layout())?;
    crate::app::groups::list_protected(&conn)
}

#[tauri::command]
pub fn protected_add(asset_ids: Vec<i64>, reason: Option<String>) -> Result<usize, String> {
    let conn = crate::infra::db::open_library(&crate::infra::library::layout())?;
    crate::app::groups::protect(&conn, &asset_ids, "manual", reason.as_deref())
}

#[tauri::command]
pub fn protected_remove(asset_ids: Vec<i64>) -> Result<usize, String> {
    let conn = crate::infra::db::open_library(&crate::infra::library::layout())?;
    crate::app::groups::unprotect(&conn, &asset_ids)
}

/// 「全部移出」（界面须二次确认，§7.6）。
#[tauri::command]
pub fn protected_remove_all() -> Result<usize, String> {
    let conn = crate::infra::db::open_library(&crate::infra::library::layout())?;
    crate::app::groups::unprotect_all(&conn)
}

#[tauri::command]
pub fn protection_stats() -> Result<ProtectionStatsDto, String> {
    let conn = crate::infra::db::open_library(&crate::infra::library::layout())?;
    let (total, week_new) = crate::app::groups::stats(&conn)?;
    Ok(ProtectionStatsDto { total, week_new })
}

/// 按「素材根 + 相对路径」加入 / 移出保护区（界面上选中的行就是相对路径，§7.6）。
#[tauri::command]
pub fn protection_toggle(
    root: String,
    rel_paths: Vec<String>,
    add: bool,
    reason: Option<String>,
) -> Result<usize, String> {
    let conn = crate::infra::db::open_library(&crate::infra::library::layout())?;
    let volume = crate::infra::volume::volume_id(std::path::Path::new(&root));
    let ids = crate::infra::db::asset_ids_by_rel(&conn, &volume, &rel_paths)?;
    if ids.is_empty() {
        return Err("选中的条目还不在索引里：先对这个文件夹扫描一次。".to_string());
    }
    if add {
        crate::app::groups::protect(&conn, &ids, "manual", reason.as_deref())
    } else {
        crate::app::groups::unprotect(&conn, &ids)
    }
}

/// 「已跳过 N 项（受保护）」明细：受保护且出现在变更集里的条目，
/// 逐条给出名称、路径、加入时间与方式（§7.6 第 1 点 / §16④）。
#[tauri::command]
pub fn skipped_protected_list() -> Result<Vec<crate::infra::db::SkippedProtectedRow>, String> {
    let conn = crate::infra::db::open_library(&crate::infra::library::layout())?;
    crate::app::groups::skipped_protected(&conn)
}

/// 「提交成功后自动把已整理素材加入保护区」开关（§10 安全分组，默认开启）。
#[tauri::command]
pub fn auto_protect_get() -> Result<bool, String> {
    let conn = crate::infra::db::open_library(&crate::infra::library::layout())?;
    Ok(crate::app::groups::auto_protect_enabled(&conn))
}

#[tauri::command]
pub fn auto_protect_set(enabled: bool) -> Result<(), String> {
    let conn = crate::infra::db::open_library(&crate::infra::library::layout())?;
    crate::infra::db::setting_set(
        &conn,
        crate::app::groups::SETTING_AUTO_PROTECT,
        if enabled { "1" } else { "0" },
    )
}

/// 已整理条目数（「已整理态 ✓」徽标与排序面板里的真值）。
#[tauri::command]
pub fn organized_count() -> Result<i64, String> {
    let conn = crate::infra::db::open_library(&crate::infra::library::layout())?;
    crate::infra::db::organized_count(&conn)
}

/// 归档计划（§7.4）：只读——算目标路径 + 冲突结论，不搬任何文件（执行属 P6）。
#[tauri::command]
pub fn archive_plan(
    root: String,
    folder: String,
    target_root: String,
    dir_template: String,
    name_template: String,
    policy: Option<String>,
    clean_empty_dirs: Option<bool>,
) -> Result<crate::app::archive::ArchivePlan, String> {
    use crate::domain::archive::ConflictPolicy;
    let conn = crate::infra::db::open_library(&crate::infra::library::layout())?;
    let policy = match policy.as_deref() {
        Some("skip") => ConflictPolicy::Skip,
        Some("abort") => ConflictPolicy::AbortBatch,
        _ => ConflictPolicy::Suffix,
    };
    crate::app::archive::plan_from_index(
        &conn,
        &root,
        &folder,
        &target_root,
        &dir_template,
        &name_template,
        policy,
        clean_empty_dirs.unwrap_or(false),
    )
}


/** 命名计划预览（§7.3）：把**指定的这些条目**按模板算成「旧名 → 新名」，只算不写。 */
#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct NamingRow {
    pub rel_path: String,
    pub old_name: String,
    pub new_name: String,
    pub notes: Vec<String>,
}

#[tauri::command]
pub fn naming_plan(
    root: String,
    rel_paths: Vec<String>,
    template: String,
    rule: crate::domain::naming::SeqRule,
) -> Result<Vec<NamingRow>, String> {
    use std::collections::HashMap;

    let conn = crate::infra::db::open_library(&crate::infra::library::layout())?;
    let volume = crate::infra::volume::volume_id(std::path::Path::new(&root));
    let rows = crate::infra::db::assets_of_volume(&conn, &volume)?;
    let by_rel: HashMap<String, &crate::infra::db::RelocateRow> = rows
        .iter()
        .map(|r| (r.rel_path.replace('/', "\\").to_lowercase(), r))
        .collect();

    // 界面给的 relPaths 是**相对素材根**的，索引里的 rel_path 是**相对卷根**的 —— 这里补上前缀，
    // 补不上再按原样试一次（两种口径都容错），否则就会出现「选中的文件一项也匹配不上」。
    let root_rel_prefix = format!(
        "{}\\",
        crate::infra::volume::rel_path_from_volume(std::path::Path::new(&root)).trim_matches('\\')
    );

    // 固定顺序（按卷内相对路径升序）→ 序号可预测，用户看到的编号每次一致
    let mut wanted: Vec<String> = rel_paths
        .iter()
        .map(|p| p.replace('/', "\\"))
        .collect();
    wanted.sort();

    let mut out = Vec::new();
    let mut seq = rule.start;
    for rel in wanted {
        let key = format!("{root_rel_prefix}{rel}").to_lowercase();
        let key = if by_rel.contains_key(&key) { key } else { rel.to_lowercase() };
        let Some(r) = by_rel.get(&key) else { continue };
        let (stem, ext) = match r.name.rsplit_once('.') {
            Some((s, e)) => (s.to_string(), e.to_string()),
            None => (r.name.clone(), String::new()),
        };
        let stem_clean = crate::domain::naming::strip_original_seq(&stem, rule.strip);
        let kind = crate::app::archive::kind_of(&ext);
        let ctx = crate::domain::naming::NameCtx {
            stem: &stem_clean,
            ext: &ext,
            camera: None,
            width: None,
            height: None,
            group: None,
            parent: None,
            kind: &kind,
            hash8: None,
            capture_time: None,
            mtime: r.mtime,
            ctime: 0,
            seq,
            counter: None,
        };
        let rendered = crate::domain::naming::render(&template, &ctx, &rule);
        out.push(NamingRow {
            rel_path: r.rel_path.clone(),
            old_name: r.name.clone(),
            new_name: rendered.name,
            notes: rendered.notes,
        });
        seq += 1;
    }
    Ok(out)
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
