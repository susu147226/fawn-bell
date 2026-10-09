/**
 * 右栏详情（§8.1 / §8.5）。
 *
 * P0 只显示「扫描出来就有的真值」：名称、路径、项数、总大小、修改时间、类型分布、云占位。
 * 整理进度 / 保护状态 / 待提交变更 / 被引用 四项的数据源分别在 P2 / P3 / P4，这里一律显示
 * `—` + 阶段标注；操作按钮按 §8.5 的清单摆出来但禁用，不做一个点了没反应的假按钮。
 */

import { CircleAlert, Cloud, Folder, Info } from 'lucide-react';
import type { ReactNode } from 'react';

import { absolutePath, dirDisplayName, subDirsOf, type ScanIndex } from '../lib/folders';
import { formatBytes, formatCount, formatDateTime, formatDuration } from '../lib/format';
import { KindIcon } from '../lib/icons';
import { KINDS, KIND_LABEL, type FileRow, type Kind, type KindCount, type ScanSummary } from '../lib/types';

export interface DetailPanelProps {
  width: number;
  index: ScanIndex | null;
  current: string;
  selected: string[];
  summary: ScanSummary | null;
}

function aggregateKinds(index: ScanIndex, rels: string[]): KindCount[] {
  const acc = new Map<Kind, { count: number; bytes: number }>();
  const bump = (k: Kind, c: number, b: number) => {
    const cur = acc.get(k) ?? { count: 0, bytes: 0 };
    cur.count += c;
    cur.bytes += b;
    acc.set(k, cur);
  };
  for (const rel of rels) {
    const d = index.dirByRel.get(rel);
    if (d) {
      for (const kc of d.kindCounts) bump(kc.kind, kc.count, kc.bytes);
      continue;
    }
    const f = index.fileByRel.get(rel);
    if (f) bump(f.kind, 1, f.size);
  }
  return KINDS.map((k) => ({ kind: k, count: acc.get(k)?.count ?? 0, bytes: acc.get(k)?.bytes ?? 0 }));
}

function KindList({ counts }: { counts: KindCount[] }) {
  const total = counts.reduce((s, k) => s + k.count, 0);
  const shown = counts.filter((k) => k.count > 0);
  if (total === 0) return <div className="tbd">这里没有素材文件。</div>;
  const max = Math.max(...shown.map((k) => k.count));
  return (
    <div className="kindlist">
      {shown.map((k) => (
        <div className="kindline" key={k.kind}>
          <KindIcon kind={k.kind} size={13} />
          <span>{KIND_LABEL[k.kind]}</span>
          <div className="bar">
            <i style={{ width: `${Math.round((k.count / max) * 100)}%` }} />
          </div>
          <span className="num">
            {formatCount(k.count)} · {formatBytes(k.bytes)}
          </span>
        </div>
      ))}
    </div>
  );
}

function Kv({ k, v, mono = false }: { k: string; v: string; mono?: boolean }) {
  return (
    <>
      <div className="kv-key">{k}</div>
      <div className={mono ? 'kv-val mono' : 'kv-val'} title={v}>
        {v}
      </div>
    </>
  );
}

/** 尚未接入的数据位统一长这样：一个破折号 + 阶段标注，绝不留空白让人以为坏了。 */
function Todo({ phase, text }: { phase: string; text: string }) {
  return (
    <div className="kv">
      <div className="kv-key">{text}</div>
      <div className="kv-val tbd">
        — <span className="tag-soon">{phase}</span>
      </div>
    </div>
  );
}

function DisabledActions({ title, items }: { title: string; items: string[] }) {
  return (
    <div className="detail-sec">
      <div className="detail-title">
        {title} <span className="tag-soon">后续阶段</span>
      </div>
      <div className="empty-actions" style={{ flexWrap: 'wrap' }}>
        {items.map((i) => (
          <button className="btn" type="button" key={i} disabled>
            {i}
          </button>
        ))}
      </div>
    </div>
  );
}

/** 只在「没选中任何项」时追加：扫描本身的元信息（当前文件夹详情里的数字不再重复一遍）。 */
function ScanExtras({ summary }: { summary: ScanSummary }) {
  return (
    <>
      <div className="detail-sec">
        <div className="detail-title">
          <Info size={14} strokeWidth={1.75} aria-hidden /> 本次扫描
        </div>
        <div className="kv">
          <Kv k="用时" v={formatDuration(summary.elapsedMs)} />
          <Kv k="跟随链接" v={summary.followedLinks ? '是' : '否（默认）'} />
          <Kv k="鹿铃库目录" v={summary.libraryDir} mono />
        </div>
      </div>
      {summary.excluded.length > 0 ? (
        <div className="detail-sec">
          <div className="detail-title">已自动排除</div>
          <div className="kv">
            {summary.excluded.map((p) => (
              <Kv k="排除" v={p} mono key={p} />
            ))}
          </div>
        </div>
      ) : null}
      {summary.bigDirs.length > 0 ? (
        <div className="detail-sec">
          <div className="detail-title">超大文件夹</div>
          <div className="kv">
            {summary.bigDirs.slice(0, 8).map((b) => (
              <Kv
                k="直接子项"
                v={`${formatCount(b.entries)} · ${b.relPath === '' ? '（素材根）' : b.relPath}`}
                mono
                key={b.relPath}
              />
            ))}
          </div>
        </div>
      ) : null}
    </>
  );
}

function FolderView({ index, rel }: { index: ScanIndex; rel: string }) {
  const d = index.dirByRel.get(rel);
  if (!d) return <SummaryFallback text="这个文件夹不在本次扫描结果里，请重新扫描。" />;
  const kids = subDirsOf(index, rel).length;

  return (
    <>
      <div className="detail-sec">
        <div className="detail-title">
          <Folder size={14} strokeWidth={1.75} aria-hidden /> 文件夹
        </div>
        <div className="detail-name">{d.name}</div>
        <div className="kv" style={{ marginTop: 'var(--spacing-2)' }}>
          <Kv k="完整路径" v={absolutePath(index, rel)} mono />
          <Kv k="直接子项" v={`${formatCount(d.directFiles + d.directDirs)} 项（文件夹 ${formatCount(d.directDirs)} · 文件 ${formatCount(d.directFiles)}）`} />
          <Kv k="含子文件夹" v={`${formatCount(d.totalFiles)} 个文件 · ${formatBytes(d.totalBytes)}`} />
          <Kv k="修改时间" v={formatDateTime(d.mtimeMs)} />
          {d.cloud ? <Kv k="云占位" v="含云端占位项" /> : null}
        </div>
      </div>

      <div className="detail-sec">
        <div className="detail-title">类型分布</div>
        <KindList counts={d.kindCounts} />
      </div>

      <div className="detail-sec">
        <div className="detail-title">整理状态</div>
        <Todo phase="P2" text="整理进度" />
        <Todo phase="P3" text="保护状态" />
        <Todo phase="P3" text="待提交变更" />
        <Todo phase="P4" text="被引用" />
      </div>

      <DisabledActions
        title="文件夹操作"
        items={['新建子文件夹', '重命名', '移动到…', '复制到…', '移出工作根']}
      />

      <div className="detail-sec">
        <div className="detail-title">
          <CircleAlert size={14} strokeWidth={1.75} aria-hidden /> 危险区
        </div>
        <div className="dangerbox">
          移入回收站（含 {formatCount(d.totalFiles)} 个文件{kids > 0 ? `、${formatCount(kids)} 个子文件夹` : ''}
          ）——默认走系统回收站，可在设置里改为直接删除。
          <div style={{ marginTop: 'var(--spacing-2)' }}>
            <button className="btn danger" type="button" disabled>
              移入回收站
            </button>{' '}
            <span className="tag-soon">P2 接入</span>
          </div>
        </div>
      </div>
    </>
  );
}

function FileView({ index, file }: { index: ScanIndex; file: FileRow }) {
  return (
    <>
      <div className="detail-sec">
        <div className="detail-title">
          <KindIcon kind={file.kind} /> 文件
        </div>
        <div className="detail-name">{file.name}</div>
        <div className="kv" style={{ marginTop: 'var(--spacing-2)' }}>
          <Kv k="类型" v={KIND_LABEL[file.kind]} />
          <Kv k="大小" v={formatBytes(file.size)} />
          <Kv k="修改时间" v={formatDateTime(file.mtimeMs)} />
          <Kv k="所在位置" v={file.relPath.slice(0, Math.max(0, file.relPath.length - file.name.length - 1)) || '（素材根）'} mono />
          <Kv k="完整路径" v={absolutePath(index, file.relPath)} mono />
        </div>
      </div>

      <div className="detail-sec">
        <div className="detail-title">整理状态</div>
        <Todo phase="P2" text="整理进度" />
        <Todo phase="P4" text="草稿" />
        <Todo phase="P3" text="保护状态" />
        <Todo phase="P4" text="被引用" />
        {file.cloud ? (
          <div className="kv">
            <div className="kv-key">云占位</div>
            <div className="kv-val">
              <Cloud size={12} strokeWidth={1.75} aria-hidden /> 是（读取内容时可能触发下载）
            </div>
          </div>
        ) : null}
      </div>

      <DisabledActions title="文件操作" items={['重命名', '移动到…', '复制到…', '整理命名…', '移入保护区']} />
      <DisabledActions title="危险区" items={['移入回收站']} />
    </>
  );
}

function BatchView({ index, rels }: { index: ScanIndex; rels: string[] }) {
  const counts = aggregateKinds(index, rels);
  const bytes = counts.reduce((s, k) => s + k.bytes, 0);
  const dirs = rels.filter((r) => index.dirByRel.has(r)).length;

  return (
    <>
      <div className="detail-sec">
        <div className="detail-title">已选 {formatCount(rels.length)} 项</div>
        <div className="kv">
          <Kv k="文件夹" v={`${formatCount(dirs)} 个`} />
          <Kv k="文件" v={`${formatCount(rels.length - dirs)} 个`} />
          <Kv k="合计大小" v={formatBytes(bytes)} />
        </div>
      </div>
      <div className="detail-sec">
        <div className="detail-title">类型分布</div>
        <KindList counts={counts} />
      </div>
      <DisabledActions
        title="批量操作"
        items={['批量重命名…', '批量移动…', '批量复制…', '加入保护区', '移出保护区']}
      />
      <DisabledActions title="危险区" items={['批量移入回收站']} />
    </>
  );
}

function SummaryFallback({ text }: { text: string }) {
  return (
    <div className="detail-sec">
      <span className="tbd">{text}</span>
    </div>
  );
}

export default function DetailPanel({ width, index, current, selected, summary }: DetailPanelProps) {
  let body: ReactNode;

  if (!index || !summary) {
    body = <SummaryFallback text="还没有扫描结果。选择素材文件夹并扫描后，这里显示详情。" />;
  } else if (selected.length > 1) {
    body = <BatchView index={index} rels={selected} />;
  } else if (selected.length === 1) {
    const rel = selected[0];
    const file = index.fileByRel.get(rel);
    body = file ? <FileView index={index} file={file} /> : <FolderView index={index} rel={rel} />;
  } else {
    const name = dirDisplayName(index, current);
    body = (
      <>
        <div className="detail-sec">
          <div className="detail-title">
            <Folder size={14} strokeWidth={1.75} aria-hidden /> 当前文件夹
          </div>
          <div className="detail-name">{name}</div>
        </div>
        <FolderView index={index} rel={current} />
        {current === '' ? <ScanExtras summary={summary} /> : null}
      </>
    );
  }

  return (
    <div className="detail" style={{ width }}>
      {body}
    </div>
  );
}
