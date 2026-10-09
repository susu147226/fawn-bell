/**
 * 状态条（§8.1 最下面一条）。
 *
 * 「待提交 / 有问题」是 **P2 起接入的真数字**（来自虚拟变更集）；「保护区」在 P4 接入，
 * 现在如实显示 0 并标注阶段——界面绝不为了好看编造数字（§12.4⑨）。
 * 「直通模式」警示位按 §9.3 固定在最右，且不随主题隐藏。
 */

import { Redo2, Undo2 } from 'lucide-react';

import { formatBytes, formatCount, formatDuration } from '../lib/format';
import type { ScanSummary } from '../lib/types';

export interface StatusBarProps {
  selectedCount: number;
  summary: ScanSummary | null;
  scanning: boolean;
  /** 变更集里的草稿数（§7.2）。 */
  draftCount: number;
  /** 其中预检有问题的条数。 */
  draftProblems: number;
  canRedo?: boolean;
  onUndo?: () => void;
  onRedo?: () => void;
}

export default function StatusBar({
  selectedCount,
  summary,
  scanning,
  draftCount,
  draftProblems,
  canRedo = false,
  onUndo,
  onRedo,
}: StatusBarProps) {
  return (
    <div className="statusbar">
      <span>
        已选 <span className="status-num">{formatCount(selectedCount)}</span> 项
      </span>
      <span className="status-sep">|</span>
      <span title="虚拟变更集：草稿只在库目录，提交前真实文件零变化（§7.2）">
        待提交 <span className="status-num">{formatCount(draftCount)}</span> 项变更（
        <span className={draftProblems > 0 ? 'status-num warn-num' : 'status-num'}>
          {formatCount(draftProblems)}
        </span>{' '}
        项有问题）
      </span>
      <span className="status-sep">|</span>
      <span title="保护区在 P4 接入">
        保护区 <span className="status-num">0</span> 项
      </span>
      <span className="status-sep">|</span>
      <button
        className="btn ghost icon"
        type="button"
        title="撤销一条草稿（Ctrl+Z）"
        disabled={draftCount === 0}
        onClick={onUndo}
      >
        <Undo2 size={14} strokeWidth={1.75} aria-hidden />
      </button>
      <button
        className="btn ghost icon"
        type="button"
        title="重做一条草稿（Ctrl+Y）"
        disabled={!canRedo}
        onClick={onRedo}
      >
        <Redo2 size={14} strokeWidth={1.75} aria-hidden />
      </button>

      <span className="passthrough-slot" title="直通模式（跳过变更集立即生效）属高级/危险设置，P9 接入">
        直通模式：关闭
        <span className="tag-soon">P9</span>
      </span>

      <span className="status-sep">|</span>
      {scanning ? (
        <span>正在扫描…</span>
      ) : summary ? (
        <span title={summary.root}>
          文件夹 <span className="status-num">{formatCount(summary.dirCount)}</span> · 文件{' '}
          <span className="status-num">{formatCount(summary.fileCount)}</span> ·{' '}
          <span className="status-num">{formatBytes(summary.totalBytes)}</span> · 用时{' '}
          <span className="status-num">{formatDuration(summary.elapsedMs)}</span>
        </span>
      ) : (
        <span>尚未扫描</span>
      )}
    </div>
  );
}
