/**
 * 与 Rust 后端的唯一通道（§12.2 分层：界面只通过命令/事件与后端讲话，不直接碰文件系统）。
 */

import { invoke } from '@tauri-apps/api/core';
import { listen, type UnlistenFn } from '@tauri-apps/api/event';
import { open } from '@tauri-apps/plugin-dialog';

import type {
  AppInfo,
  AssetMeta,
  DedupeReport,
  DraftList,
  DraftOp,
  LibraryInfo,
  Projection,
  RelocatePlan,
  ScanDone,
  ScanProgress,
  ScanResult,
  ScanSnapshot,
} from './types';

export const api = {
  appInfo: () => invoke<AppInfo>('app_info'),

  /** 取素材缩略图（PNG 的 data URL）；拿不到返回 null，界面降级为类型图标（§12.3）。 */
  thumb: (path: string) => invoke<string | null>('thumb_data_url', { path }),

  /** 取索引里的元数据（EXIF / Shell 属性）；不在索引里返回 null。 */
  assetMeta: (root: string, relPath: string) =>
    invoke<AssetMeta | null>('asset_meta', { root, relPath }),

  /** 库位置信息（设置 / 重定位向导用）。 */
  libraryInfo: () => invoke<LibraryInfo>('library_info'),

  /** 内容去重报告（§7.12）；判定与「重复内容」集合同源。 */
  dedupeReport: (policy?: string) => invoke<DedupeReport>('dedupe_report', { policy: policy ?? null }),

  /** 重新定位：只出计划，不改索引。 */
  relocatePlan: (oldRoot: string, newRoot: string) =>
    invoke<RelocatePlan>('relocate_plan', { oldRoot, newRoot }),

  /** 重新定位：应用（高置信一律改写；待确认需 includeConfirm=true）。 */
  relocateApply: (oldRoot: string, newRoot: string, includeConfirm: boolean) =>
    invoke<number>('relocate_apply', { oldRoot, newRoot, includeConfirm }),

  /* ── 虚拟变更集（§7.2）：草稿只在库目录，提交前不碰磁盘 ── */
  draftList: () => invoke<DraftList>('draft_list'),
  draftAdd: (
    op: DraftOp,
    src: string,
    dst: string | null = null,
    assetId: number | null = null,
  ) => invoke<DraftList>('draft_add', { op, src, dst, assetId }),
  draftUndo: () => invoke<DraftList>('draft_undo'),
  draftRedo: () => invoke<DraftList>('draft_redo'),
  draftClear: () => invoke<DraftList>('draft_clear'),
  draftProject: (paths: string[]) => invoke<Projection[]>('draft_project', { paths }),

  /** 开始一次只读扫描；立即返回 scanId，进度与结束走事件。 */
  scanStart: (root: string, followLinks = false) =>
    invoke<number>('scan_start', { root, followLinks }),

  /** 请求取消；返回是否有任务被取消。 */
  scanCancel: () => invoke<boolean>('scan_cancel'),

  /** 取某次扫描的明细。 */
  scanResult: (scanId: number) => invoke<ScanResult>('scan_result', { scanId }),

  /** 界面重载后恢复进度显示。 */
  scanSnapshot: () => invoke<ScanSnapshot>('scan_snapshot'),
};

/** 打开系统「选择文件夹」对话框；取消返回 null。 */
export async function pickFolder(): Promise<string | null> {
  const picked = await open({
    directory: true,
    multiple: false,
    title: '选择素材文件夹',
  });
  return typeof picked === 'string' ? picked : null;
}

export function onScanProgress(cb: (p: ScanProgress) => void): Promise<UnlistenFn> {
  return listen<ScanProgress>('scan://progress', (e) => cb(e.payload));
}

export function onScanDone(cb: (d: ScanDone) => void): Promise<UnlistenFn> {
  return listen<ScanDone>('scan://done', (e) => cb(e.payload));
}

/** 把后端抛出的错误统一成可展示的中文文案。 */
export function errorText(e: unknown): string {
  if (typeof e === 'string') return e;
  if (e instanceof Error) return e.message;
  try {
    return JSON.stringify(e);
  } catch {
    return String(e);
  }
}
