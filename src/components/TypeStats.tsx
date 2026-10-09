/** 顶部统计（§7.1：图片 1,820 · 视频 96 …），只显示扫描到的类型。 */

import { KindIcon } from '../lib/icons';
import { formatBytes, formatCount } from '../lib/format';
import { KIND_LABEL, type KindStat } from '../lib/types';

export interface TypeStatsProps {
  byKind: KindStat[];
}

export default function TypeStats({ byKind }: TypeStatsProps) {
  const shown = byKind.filter((k) => k.count > 0);
  if (shown.length === 0) {
    return <div className="subbar">这个文件夹里没有素材</div>;
  }

  return (
    <div className="subbar">
      {shown.map((k, i) => (
        <span className="stat" key={k.kind}>
          {i > 0 ? <span className="stat-sep">·</span> : null}
          <KindIcon kind={k.kind} />
          {KIND_LABEL[k.kind]} <span className="stat-num">{formatCount(k.count)}</span>
          <span className="stat-sep">（{formatBytes(k.bytes)}）</span>
        </span>
      ))}
    </div>
  );
}
