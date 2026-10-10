/**
 * 扫描进度条（§7.1：长任务可取消、显示已浏览数量与当前路径）。
 *
 * 进度比例只用真实数据算：`已遍历文件夹 / 已进入队列的文件夹`。
 * 遍历前不预扫目录，所以没有「总数」这个概念，界面也不编一个假的百分比分母。
 */

import { X } from 'lucide-react';

import { formatCount, formatEta } from '../lib/format';
import type { ScanProgress } from '../lib/types';

export interface ScanBannerProps {
  progress: ScanProgress;
  onCancel: () => void;
}

export default function ScanBanner({ progress, onCancel }: ScanBannerProps) {
  const entered = progress.dirsDone + progress.dirsPending;
  const pct = entered > 0 ? Math.round((progress.dirsDone / entered) * 100) : 0;

  return (
    <div className="scan-banner">
      <span className="scan-text">
        {progress.phase}：已浏览 <span className="status-num">{formatCount(progress.files)}</span> 个文件 · 文件夹{' '}
        <span className="status-num">
          {formatCount(progress.dirsDone)}/{formatCount(entered)}
        </span>{' '}
        · {formatEta(progress.etaMs)}
      </span>
      <div className="scan-track" role="progressbar" aria-valuemin={0} aria-valuemax={100} aria-valuenow={pct}>
        <div className="scan-fill" style={{ width: `${pct}%` }} />
      </div>
      <span className="scan-current">
        {progress.current}
      </span>
      <button className="btn" type="button" onClick={onCancel}>
        <X size={14} strokeWidth={1.75} aria-hidden />
        取消
      </button>
    </div>
  );
}
