/**
 * 目录索引（纯函数）。
 *
 * §8.5 性能条款：文件夹行的项数 / 总大小 / 整理进度必须是现成聚合值，渲染每一行都不能去统计磁盘。
 * 这里在扫描结果之上一次性建好索引，界面只做 O(1) 查表。
 */

import { compareName } from './format';
import type { DirAgg, FileRow, ScanResult } from './types';

export interface ScanIndex {
  root: string;
  dirByRel: Map<string, DirAgg>;
  childDirs: Map<string, DirAgg[]>;
  childFiles: Map<string, FileRow[]>;
  /** 相对路径 → 文件行（选中项与详情面板的 O(1) 查表）。 */
  fileByRel: Map<string, FileRow>;
  /** 已按自然序排好的全部目录（供搜索与统计用）。 */
  allDirs: DirAgg[];
  allFiles: FileRow[];
  /** 每个目录的后代总大小（根目录即全库大小）。 */
  dirCount: number;
  fileCount: number;
}

/** 取父目录相对路径；顶层项的父目录是素材根（空串）。 */
export function parentOf(relPath: string): string {
  if (relPath === '') return '';
  const i = relPath.lastIndexOf('/');
  return i < 0 ? '' : relPath.slice(0, i);
}

export function buildIndex(result: ScanResult): ScanIndex {
  const dirByRel = new Map<string, DirAgg>();
  const childDirs = new Map<string, DirAgg[]>();
  const childFiles = new Map<string, FileRow[]>();

  for (const d of result.dirs) {
    dirByRel.set(d.relPath, d);
    if (d.relPath === '') continue;
    const p = parentOf(d.relPath);
    const list = childDirs.get(p);
    if (list) list.push(d);
    else childDirs.set(p, [d]);
  }

  const fileByRel = new Map<string, FileRow>();
  for (const f of result.files) {
    fileByRel.set(f.relPath, f);
    const p = parentOf(f.relPath);
    const list = childFiles.get(p);
    if (list) list.push(f);
    else childFiles.set(p, [f]);
  }

  for (const list of childDirs.values()) {
    list.sort((a, b) => compareName(a.name, b.name));
  }
  for (const list of childFiles.values()) {
    list.sort((a, b) => compareName(a.name, b.name));
  }

  return {
    root: result.summary.root,
    dirByRel,
    childDirs,
    childFiles,
    fileByRel,
    allDirs: result.dirs,
    allFiles: result.files,
    dirCount: result.summary.dirCount,
    fileCount: result.summary.fileCount,
  };
}

/** 取某目录的直接子文件夹（已排序，缺省空数组）。 */
export function subDirsOf(index: ScanIndex, relPath: string): DirAgg[] {
  return index.childDirs.get(relPath) ?? [];
}

/** 取某目录的直接子文件（已排序，缺省空数组）。 */
export function filesOf(index: ScanIndex, relPath: string): FileRow[] {
  return index.childFiles.get(relPath) ?? [];
}

/** 展示用目录名：素材根没有名字，用绝对路径末段（§8.1 工作根标签页）。 */
export function dirDisplayName(index: ScanIndex, relPath: string): string {
  if (relPath !== '') return relPath.slice(relPath.lastIndexOf('/') + 1);
  const root = index.root.replace(/[\\/]+$/, '');
  const i = Math.max(root.lastIndexOf('\\'), root.lastIndexOf('/'));
  return i >= 0 ? root.slice(i + 1) : root;
}

/** 素材根 + 相对路径 → 绝对路径（界面上只展示，不做任何写操作）。 */
export function absolutePath(index: ScanIndex, relPath: string): string {
  if (relPath === '') return index.root;
  const sep = index.root.includes('\\') ? '\\' : '/';
  const head = index.root.replace(/[\\/]+$/, '');
  return `${head}${sep}${relPath.split('/').join(sep)}`;
}

/** 某个目录的类型分布（含 0 项，顺序稳定）。 */
export function kindCountsOf(index: ScanIndex, relPath: string) {
  const d = index.dirByRel.get(relPath);
  if (!d) return [];
  return d.kindCounts;
}

/** 面包屑层级：素材根 → … → 当前目录。 */
export function breadcrumbOf(relPath: string): { label: string; rel: string }[] {
  const crumbs = [{ label: '', rel: '' }];
  if (relPath === '') return crumbs;
  const parts = relPath.split('/');
  let acc = '';
  for (const p of parts) {
    acc = acc === '' ? p : `${acc}/${p}`;
    crumbs.push({ label: p, rel: acc });
  }
  return crumbs;
}
