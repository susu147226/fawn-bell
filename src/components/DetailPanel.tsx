/**
 * 右栏详情（§8.1 / §8.5）。
 *
 * P0 只显示「扫描出来就有的真值」：名称、路径、项数、总大小、修改时间、类型分布、云占位。
 * 整理进度 / 保护状态 / 待提交变更 / 被引用 四项的数据源分别在 P2 / P3 / P4，这里一律显示
 * `—` + 阶段标注；操作按钮按 §8.5 的清单摆出来但禁用，不做一个点了没反应的假按钮。
 */

import { CircleAlert, Cloud, Folder, Info } from 'lucide-react';
import { useEffect, useState, type ReactNode } from 'react';

import { absolutePath, dirDisplayName, subDirsOf, type ScanIndex } from '../lib/folders';
import { formatBytes, formatCount, formatDateTime, formatDuration } from '../lib/format';
import { KindIcon } from '../lib/icons';
import { api, errorText } from '../lib/ipc';
import NamingPanel from './NamingPanel';
import ProtectionPanel from './ProtectionPanel';
import { DedupePanel, DraftsPanel, RelocateWizard, type NoticePayload } from './Panels';
import {
  KINDS,
  KIND_LABEL,
  type AssetMeta,
  type FileRow,
  type Kind,
  type KindCount,
  type ScanSummary,
} from '../lib/types';

/** 右栏可切换的工具面板。 */
export type PanelKind = 'none' | 'dedupe' | 'relocate' | 'drafts' | 'naming' | 'protection';

/** Shell 属性键 → 中文标签（§6.4）。 */
const PROP_LABEL: Record<string, string> = {
  'System.Media.Duration': '时长',
  'System.Video.FrameWidth': '视频宽',
  'System.Video.FrameHeight': '视频高',
  'System.Video.EncodingBitrate': '视频码率',
  'System.Video.TotalBitrate': '总码率',
  'System.Music.Artist': '艺术家',
  'System.Title': '标题',
  'System.Author': '作者',
  'System.ItemTypeText': '系统类型',
};

/** `System.Media.Duration` 是 100 ns 单位，展示时换算成秒。 */
function propText(key: string, value: string): string {
  if (key === 'System.Media.Duration') {
    const n = Number(value);
    return Number.isFinite(n) ? `${formatDuration(n / 10_000)}` : value;
  }
  return value;
}

/** 缩略图（§12.3：走系统缩略图；拿不到就降级，绝不编造图形）。 */
function FilePreview({ abs, kind }: { abs: string; kind: Kind }) {
  const [url, setUrl] = useState<string | null>(null);
  const [state, setState] = useState<'loading' | 'ok' | 'none'>('loading');

  useEffect(() => {
    let alive = true;
    setState('loading');
    setUrl(null);
    api
      .thumb(abs)
      .then((u) => {
        if (!alive) return;
        if (u) {
          setUrl(u);
          setState('ok');
        } else {
          setState('none');
        }
      })
      .catch(() => {
        if (alive) setState('none');
      });
    return () => {
      alive = false;
    };
  }, [abs, kind]);

  if (state === 'loading') return <div className="tbd">正在生成缩略图…</div>;
  if (state === 'none' || !url) {
    return <div className="tbd">系统没有提供这个文件的缩略图，已降级为类型图标。</div>;
  }
  return (
    <img
      src={url}
      alt=""
      style={{
        width: '100%',
        maxWidth: 'calc(var(--thumb-size) * 2)',
        maxHeight: 'calc(var(--thumb-size) * 2)',
        objectFit: 'contain',
        background: 'var(--surface-alt)',
        border: '1px solid var(--border)',
        borderRadius: 'var(--radius-md)',
      }}
    />
  );
}

/** 索引里的元数据（§6.4）：EXIF 的尺寸/拍摄时间/机型/方向/GPS + Shell 扩展属性。 */
function FileMetaRows({ root, relPath }: { root: string; relPath: string }) {
  const [meta, setMeta] = useState<AssetMeta | null>(null);
  const [loaded, setLoaded] = useState(false);

  useEffect(() => {
    let alive = true;
    setLoaded(false);
    api
      .assetMeta(root, relPath)
      .then((m) => {
        if (alive) {
          setMeta(m);
          setLoaded(true);
        }
      })
      .catch(() => {
        if (alive) setLoaded(true);
      });
    return () => {
      alive = false;
    };
  }, [root, relPath]);

  if (!loaded) return <div className="tbd">正在读取索引…</div>;
  if (!meta) return <div className="tbd">这个文件还没有进索引（扫描一次就会出现）。</div>;

  const rows: ReactNode[] = [];
  if (meta.width && meta.height) {
    rows.push(<Kv k="尺寸" v={`${formatCount(meta.width)} × ${formatCount(meta.height)}`} key="wh" />);
  }
  if (meta.captureTime) rows.push(<Kv k="拍摄时间" v={formatDateTime(meta.captureTime)} key="ct" />);
  if (meta.camera) rows.push(<Kv k="机型" v={meta.camera} key="cam" />);
  if (meta.orientation) rows.push(<Kv k="方向" v={`EXIF ${meta.orientation}`} key="ori" />);
  if (meta.gpsLat != null && meta.gpsLon != null) {
    rows.push(<Kv k="GPS" v={`${meta.gpsLat.toFixed(5)}, ${meta.gpsLon.toFixed(5)}`} key="gps" />);
  }
  for (const p of meta.props) {
    rows.push(<Kv k={PROP_LABEL[p.key] ?? p.key} v={propText(p.key, p.value)} key={p.key} />);
  }
  if (meta.missing) {
    rows.push(<Kv k="索引状态" v="文件已不在磁盘上（元数据仍保留，可重新扫描或从索引移除）" key="missing" />);
  }
  return rows.length > 0 ? (
    <div className="kv">{rows}</div>
  ) : (
    <div className="tbd">索引里没有这个文件的额外元数据（无 EXIF，系统也未提供属性）。</div>
  );
}

export interface DetailPanelProps {
  width: number;
  index: ScanIndex | null;
  current: string;
  selected: string[];
  summary: ScanSummary | null;
  /** 当前显示的 P1 工具面板；`none` 时显示常规详情。 */
  panel: PanelKind;
  onPanel: (p: PanelKind) => void;
  onNotice: (n: NoticePayload) => void;
  /** 保护区发生变化（加入 / 移出）时通知外层刷新左栏计数（§7.6）。 */
  onProtectionChanged?: () => void;
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

function FileView({
  index,
  file,
  root,
  onNotice,
}: {
  index: ScanIndex;
  file: FileRow;
  root: string;
  onNotice: (n: NoticePayload) => void;
}) {
  const [newName, setNewName] = useState('');

  /** 排一条重命名草稿（§7.2：只进变更集，磁盘一个字节都不动）。 */
  const addDraft = async () => {
    const abs = absolutePath(index, file.relPath);
    const dir = abs.slice(0, Math.max(0, abs.length - file.name.length));
    try {
      const list = await api.draftAdd('rename', abs, dir + newName.trim());
      onNotice({
        kind: list.problems > 0 ? 'warn' : 'info',
        title: '已排入变更集',
        lines: [
          `${file.name} → ${newName.trim()}`,
          list.problems > 0
            ? `当前有 ${formatCount(list.problems)} 条草稿需要处理，详见右栏「变更集…」`
            : '磁盘没有任何改动；按 Ctrl+Z 可以撤销这条草稿。',
        ],
      });
      setNewName('');
    } catch (e) {
      onNotice({ kind: 'error', title: '排入草稿失败', lines: [errorText(e)] });
    }
  };

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

      {/* §8.1 设计稿：右栏顺序是「预览 → 字段 → 待提交变更」，预览在最上（原先被排在最后） */}
      <div className="detail-sec">
        <div className="detail-title">预览</div>
        <FilePreview abs={absolutePath(index, file.relPath)} kind={file.kind} />
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

      <div className="detail-sec">
        <div className="detail-title">排入变更集（草稿）</div>
        <div className="kv">
          <div className="kv-key">新名字</div>
          <div className="kv-val">
            <input
              type="text"
              value={newName}
              placeholder={file.name}
              onChange={(e) => setNewName(e.target.value)}
              style={{ width: '100%' }}
            />
            <div style={{ marginTop: 'var(--spacing-2)' }}>
              <button
                className="btn"
                type="button"
                disabled={!newName.trim() || newName.trim() === file.name}
                onClick={() => void addDraft()}
              >
                加入草稿（重命名）
              </button>
              <span className="tbd"> 只进变更集，磁盘不动</span>
            </div>
          </div>
        </div>
      </div>

      <div className="detail-sec">
        <div className="detail-title">元数据（索引）</div>
        <FileMetaRows root={root} relPath={file.relPath} />
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

/** 常规详情底部的两个 P1 工具入口（去重集合、重定位向导）。 */
function ToolsEntry({ onPanel }: { onPanel: (p: PanelKind) => void }) {
  return (
    <div className="detail-sec">
      <div className="detail-title">工具</div>
      <div className="empty-actions" style={{ flexWrap: 'wrap' }}>
        <button className="btn" type="button" onClick={() => onPanel('naming')}>
          命名规则…
        </button>
        <button className="btn" type="button" onClick={() => onPanel('protection')}>
          保护区…
        </button>
        <button className="btn" type="button" onClick={() => onPanel('drafts')}>
          变更集…
        </button>
        <button className="btn" type="button" onClick={() => onPanel('dedupe')}>
          重复内容…
        </button>
        <button className="btn" type="button" onClick={() => onPanel('relocate')}>
          重新定位素材树…
        </button>
      </div>
    </div>
  );
}

export default function DetailPanel({
  width,
  index,
  current,
  selected,
  summary,
  panel,
  onPanel,
  onNotice,
  onProtectionChanged,
}: DetailPanelProps) {
  let body: ReactNode;

  if (panel === 'dedupe') {
    body = <DedupePanel onClose={() => onPanel('none')} />;
  } else if (panel === 'drafts') {
    body = <DraftsPanel onClose={() => onPanel('none')} />;
  } else if (panel === 'naming') {
    body = <NamingPanel onClose={() => onPanel('none')} />;
  } else if (panel === 'protection') {
    body = <ProtectionPanel onClose={() => onPanel('none')} onChanged={onProtectionChanged} />;
  } else if (panel === 'relocate') {
    body = (
      <RelocateWizard root={summary?.root ?? null} onClose={() => onPanel('none')} onNotice={onNotice} />
    );
  } else if (!index || !summary) {
    body = <SummaryFallback text="还没有扫描结果。选择素材文件夹并扫描后，这里显示详情。" />;
  } else if (selected.length > 1) {
    body = <BatchView index={index} rels={selected} />;
  } else if (selected.length === 1) {
    const rel = selected[0];
    const file = index.fileByRel.get(rel);
    body = file ? (
      <FileView index={index} file={file} root={summary.root} onNotice={onNotice} />
    ) : (
      <FolderView index={index} rel={rel} />
    );
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
      {panel === 'none' ? <ToolsEntry onPanel={onPanel} /> : null}
    </div>
  );
}
