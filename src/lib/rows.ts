/**
 * 中栏混排列表的行模型（§8.5 默认子模式）。
 *
 * 约定：当前目录的文件夹行在前、文件行在后（与资源管理器一致，用户不必二次适应）；
 * 文件夹行可原地展开一层，只列子文件夹与子项计数摘要，不做无限层级（§8.5 指定）。
 * 展开出的子文件夹行只用于跳转，不再继续展开。
 */

import { filesOf, subDirsOf, type ScanIndex } from './folders';
import { formatCount } from './format';
import type { DirAgg, FileRow } from './types';

export type ListRow =
  | {
      type: 'dir';
      /** 行的稳定 key，也是选择集里的标识（目录用 relPath）。 */
      id: string;
      relPath: string;
      dir: DirAgg;
      depth: 0 | 1;
      expandable: boolean;
      expanded: boolean;
    }
  | { type: 'file'; id: string; relPath: string; file: FileRow; depth: 0 | 1 }
  | { type: 'note'; id: string; depth: 0 | 1; text: string };

/** 展开一层时，子文件夹之后的「另有 N 个文件」提示行。 */
export function noteId(relPath: string): string {
  return `note:${relPath}`;
}

export function buildRows(index: ScanIndex, current: string, expanded: Set<string>): ListRow[] {
  const rows: ListRow[] = [];

  for (const d of subDirsOf(index, current)) {
    const kids = subDirsOf(index, d.relPath);
    const isOpen = expanded.has(d.relPath);
    rows.push({
      type: 'dir',
      id: d.relPath,
      relPath: d.relPath,
      dir: d,
      depth: 0,
      expandable: kids.length > 0,
      expanded: isOpen,
    });

    if (!isOpen) continue;

    for (const sd of kids) {
      rows.push({
        type: 'dir',
        id: sd.relPath,
        relPath: sd.relPath,
        dir: sd,
        depth: 1,
        expandable: false,
        expanded: false,
      });
    }
    const subFiles = filesOf(index, d.relPath);
    if (subFiles.length > 0) {
      rows.push({
        type: 'note',
        id: noteId(d.relPath),
        depth: 1,
        text: `另有 ${formatCount(subFiles.length)} 个文件（展开一层只列子文件夹）`,
      });
    }
  }

  for (const f of filesOf(index, current)) {
    rows.push({ type: 'file', id: f.relPath, relPath: f.relPath, file: f, depth: 0 });
  }

  return rows;
}
