/**
 * 中栏卡片网格视图（§8.1 / §8.5；设计稿 01-main-light 里的「网格视图」）。
 *
 * 与列表视图同源：同一份 `rows`、同一份投影、同一套选中逻辑，只是换个画法——
 * 不存在「两套渲染各算各的」。
 *
 * 缩略图走既有命令 `thumb_data_url`（系统缩略图 → 库内缓存）；拿不到就显示类型图标，
 * **不编造图形**（§12.3）。卡片角标只画算得出来的真值：草稿态来自虚拟变更集投影。
 */

import { useEffect, useState } from 'react';
import { Folder } from 'lucide-react';

import { absolutePath, type ScanIndex } from '../lib/folders';
import { formatBytes, formatCount } from '../lib/format';
import { KindIcon } from '../lib/icons';
import { api } from '../lib/ipc';
import type { ListRow } from '../lib/rows';
import { KIND_LABEL, type Kind, type Projection } from '../lib/types';

export interface AssetGridProps {
  index: ScanIndex;
  rows: ListRow[];
  selection: Set<string>;
  drafts?: Map<string, Projection>;
  onSelect: (id: string, mode: 'single' | 'toggle' | 'range') => void;
  onOpenDir: (relPath: string) => void;
}

/** 缩略图缓存：同一会话内同一路径只取一次（§10：按需取图，不做全量预生成）。 */
const thumbCache = new Map<string, string | null>();

function CardThumb({ abs, kind }: { abs: string; kind: Kind }) {
  const [url, setUrl] = useState<string | null>(thumbCache.get(abs) ?? null);
  const [state, setState] = useState<'idle' | 'ok' | 'none'>(
    thumbCache.has(abs) ? (thumbCache.get(abs) ? 'ok' : 'none') : 'idle',
  );

  useEffect(() => {
    if (thumbCache.has(abs)) return;
    let alive = true;
    api
      .thumb(abs)
      .then((u) => {
        thumbCache.set(abs, u);
        if (!alive) return;
        setUrl(u);
        setState(u ? 'ok' : 'none');
      })
      .catch(() => {
        thumbCache.set(abs, null);
        if (alive) setState('none');
      });
    return () => {
      alive = false;
    };
  }, [abs]);

  if (state === 'ok' && url) {
    return <img className="card-thumb-img" src={url} alt="" style={{ width: '100%', height: '100%', objectFit: 'cover' }} />;
  }
  return <KindIcon kind={kind} size={32} />;
}

export default function AssetGrid({
  index,
  rows,
  selection,
  drafts,
  onSelect,
  onOpenDir,
}: AssetGridProps) {
  if (rows.length === 0) {
    return (
      <div className="grid-empty">
        <span className="tbd">无素材</span>
      </div>
    );
  }

  return (
    <div
      style={{
        display: 'grid',
        gridTemplateColumns: 'repeat(auto-fill, minmax(calc(var(--thumb-size) + var(--spacing-5)), 1fr))',
        gap: 'var(--spacing-3)',
        padding: 'var(--spacing-3)',
        overflow: 'auto',
        alignContent: 'start',
      }}
    >
      {rows.map((row) => {
        // 网格视图不显示「另有 N 个文件」这类提示行（它是列表视图的展开层附属品）
        if (row.type === 'note') return null;
        const isDir = row.type === 'dir';
        const relPath = row.relPath;
        const name = isDir ? row.dir.name : row.file.name;
        const proj = isDir ? undefined : drafts?.get(relPath.toLowerCase());
        const displayName = proj?.drafted
          ? proj.path.split(/[\\/]/).filter(Boolean).pop() ?? name
          : name;
        const selected = selection.has(row.id);

        return (
          <div
            key={row.id}
            className={selected ? 'card selected' : 'card'}
            style={{
              border: '1px solid var(--border)',
              borderRadius: 'var(--radius-lg)',
              overflow: 'hidden',
              background: 'var(--surface)',
              outline: selected ? '2px solid var(--accent)' : 'none',
              cursor: 'pointer',
            }}
           
            onClick={(e) => onSelect(row.id, e.shiftKey ? 'range' : e.ctrlKey || e.metaKey ? 'toggle' : 'single')}
            onDoubleClick={() => {
              if (isDir) onOpenDir(relPath);
            }}
          >
            <div
              style={{
                display: 'flex',
                alignItems: 'center',
                justifyContent: 'center',
                height: 'var(--thumb-size)',
                background: 'var(--bg)',
              }}
            >
              {isDir ? (
                <Folder size={32} strokeWidth={1.5} aria-hidden />
              ) : (
                <CardThumb abs={absolutePath(index, relPath)} kind={row.file.kind} />
              )}
            </div>
            <div style={{ padding: 'var(--spacing-2)' }}>
              <div
                className="card-name"
                style={{ fontStyle: proj?.drafted ? 'italic' : undefined, wordBreak: 'break-all' }}
              >
                {displayName}
                {proj?.drafted ? (
                  <span
                    aria-hidden
                    style={{
                      display: 'inline-block',
                      width: 'var(--spacing-1)',
                      height: 'var(--spacing-1)',
                      marginLeft: 'var(--spacing-1)',
                      borderRadius: 'var(--radius-lg)',
                      background: 'var(--draft-mark)',
                    }}
                  />
                ) : null}
              </div>
              <div style={{ display: 'flex', alignItems: 'center', gap: 'var(--spacing-1)' }}>
                {isDir ? (
                  <span className="tbd">
                    {formatCount(row.dir.directFiles + row.dir.directDirs)} 项 · {formatBytes(row.dir.totalBytes)}
                  </span>
                ) : (
                  <>
                    <KindIcon kind={row.file.kind} size={12} />
                    <span className="tbd">
                      {KIND_LABEL[row.file.kind]} · {formatBytes(row.file.size)}
                    </span>
                  </>
                )}
                {proj?.drafted ? <span className="badge">{proj.removed ? '待删除' : '草稿'}</span> : null}
                {!isDir && row.file.cloud ? <span className="badge">云端</span> : null}
              </div>
            </div>
          </div>
        );
      })}
    </div>
  );
}
