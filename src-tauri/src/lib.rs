//! 鹿铃 · Tauri 外壳装配。
//!
//! 分层（执行版 §12.2）：
//! - [`domain`] 核心域（纯逻辑、无 IO、可穷举单测）
//! - [`infra`] 基础设施层（FS 适配器、路径解析）
//! - [`app`] 应用服务层（`ScanUseCase` 等）
//! - [`ipc`] UI 层与后端之间的命令 / 事件边界
//! - [`cli`] CLI 入口（引擎可独立驱动）

pub mod app;
pub mod cli;
pub mod domain;
pub mod infra;
pub mod ipc;

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(ipc::ScanState::new())
        .manage(app::drafts::DraftState::new())
        .invoke_handler(tauri::generate_handler![
            ipc::app_info,
            ipc::scan_start,
            ipc::scan_cancel,
            ipc::scan_result,
            ipc::scan_snapshot,
            ipc::thumb_data_url,
            ipc::asset_meta,
            ipc::library_info,
            ipc::dedupe_report,
            ipc::relocate_plan,
            ipc::relocate_apply,
            ipc::draft_list,
            ipc::draft_add,
            ipc::draft_undo,
            ipc::draft_redo,
            ipc::draft_clear,
            ipc::draft_project,
            ipc::preset_list,
            ipc::preset_save,
            ipc::preset_delete,
            ipc::preset_reorder,
            ipc::preset_export,
            ipc::preset_import,
            ipc::preset_apply,
            ipc::naming_preview,
        ])
        .run(tauri::generate_context!())
        .expect("鹿铃启动失败");
}
