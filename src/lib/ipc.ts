/**
 * 与 Rust 后端的唯一通道（§12.2 分层：界面只通过命令/事件与后端讲话，不直接碰文件系统）。
 */

import { invoke } from '@tauri-apps/api/core';
import { listen, type UnlistenFn } from '@tauri-apps/api/event';
import { open } from '@tauri-apps/plugin-dialog';

import type { AppInfo, ScanDone, ScanProgress, ScanResult, ScanSnapshot } from './types';

export const api = {
  appInfo: () => invoke<AppInfo>('app_info'),

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
