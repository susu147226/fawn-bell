/**
 * 状态条（§8.1 最下面一条）。
 *
 * 「待提交变更」「有问题」「保护区」三项在 P0 恒为 0 并标注接入阶段——
 * 界面绝不为了好看编造数字（§12.4⑨ 界面绝不显示假数据）。
 * 「直通模式」警示位按 §9.3 固定在最右，且不随主题隐藏。
 */

import { Redo2, Undo2 } from 'lucide-react';

import { formatBytes, formatCount, formatDuration } from '../lib/format';
import type { ScanSummary } from '../lib/types';

export interface StatusBarProps {
  selectedCount: number;
  summary: ScanSummary | null;
  scanning: boolean;
}

export default function StatusBar({ selectedCount, summary, scanning }: StatusBarProps) {
  return (
    <div className="statusbar">
      <span>
        已选 <span className="status-num">{formatCount(selectedCount)}</span> 项
      </span>
      <span className="status-sep">|</span>
      <span title="变更单与撤销栈在 P3 接入">
        待提交 <span className="status-num">0</span> 项变更（<span className="status-num">0</span> 项有问题）
      </span>
      <span className="status-sep">|</span>
      <span title="保护区在 P3 接入">
        保护区 <span className="status-num">0</span> 项
      </span>
      <span className="status-sep">|</span>
      <button className="btn ghost icon" type="button" title="撤销（P3 接入）" disabled>
        <Undo2 size={14} strokeWidth={1.75} aria-hidden />
      </button>
      <button className="btn ghost icon" type="button" title="重做（P3 接入）" disabled>
        <Redo2 size={14} strokeWidth={1.75} aria-hidden />
      </button>

      <span className="passthrough-slot" title="直通模式（软件内不接管真实文件夹）在 P2 接入">
        直通模式：关闭
        <span className="tag-soon">P2</span>
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
