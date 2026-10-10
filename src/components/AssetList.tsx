/**
 * 中栏混排列表（§8.5 文件夹视图默认子模式）。
 *
 * 列：名称｜类型｜项数 / 大小｜整理进度｜修改时间｜状态。
 * 其中「整理进度」「草稿 / 保护 / 被引用」四列的数据源（P2/P3/P4）还没接上，
 * P0 只画空槽并标 `—`，不填任何假值；「云占位」是现在就算得出来的真数据。
 *
 * 行高取自 tokens 的 `--row-height`，不在 TS 里再写一份像素常量（§12.4②）。
 * 长列表用虚拟滚动（§8.5 性能条款）。
 */

import { useVirtualizer } from '@tanstack/react-virtual';
import { ChevronDown, ChevronRight, Cloud, Folder } from 'lucide-react';
import { useEffect, useMemo, useRef, useState, type KeyboardEvent as ReactKeyboardEvent, type MouseEvent } from 'react';

import { formatBytes, formatCount, formatTime } from '../lib/format';
import { KindIcon } from '../lib/icons';
import type { ListRow } from '../lib/rows';
import { KIND_LABEL, type Projection } from '../lib/types';

export interface AssetListProps {
  rows: ListRow[];
  selection: Set<string>;
  activeId: string | null;
  /** 投影结果（键＝根内相对路径的小写形式）：草稿态行显示新名字 + 斜体 + 橙点（§7.2）。 */
  drafts?: Map<string, Projection>;
  onSelect: (id: string, mode: 'single' | 'toggle' | 'range') => void;
  onOpenDir: (relPath: string) => void;
  onToggleExpand: (relPath: string) => void;
  onMoveActive: (delta: number | 'home' | 'end') => void;
  onToggleActive: () => void;
  /** 把拖动的行交给外层（内容为素材根内相对路径）。 */
  onDragStartRows?: (rels: string[]) => void;
}

/** 行高单一真源：读 tokens.css 的 `--row-height`。 */
function readRowHeight(): number {
  const raw = getComputedStyle(document.documentElement).getPropertyValue('--row-height');
  const v = Number.parseFloat(raw);
  return Number.isFinite(v) && v > 0 ? v : 28;
}

const HEADERS = ['名称', '类型', '项数 / 大小', '整理进度', '修改时间', '状态'];

export default function AssetList({
  rows,
  selection,
  activeId,
  drafts,
  onSelect,
  onOpenDir,
  onToggleExpand,
  onMoveActive,
  onToggleActive,
  onDragStartRows,
}: AssetListProps) {
  const scrollRef = useRef<HTMLDivElement | null>(null);
  const [rowHeight] = useState(readRowHeight);

  const virtualizer = useVirtualizer({
    count: rows.length,
    getScrollElement: () => scrollRef.current,
    estimateSize: () => rowHeight,
    overscan: 12,
  });

  useEffect(() => {
    if (!activeId) return;
    const idx = rows.findIndex((r) => r.id === activeId);
    if (idx >= 0) virtualizer.scrollToIndex(idx, { align: 'auto' });
  }, [activeId, rows, virtualizer]);

  const activeIndex = useMemo(
    () => (activeId ? rows.findIndex((r) => r.id === activeId) : -1),
    [activeId, rows],
  );

  const onKeyDown = (e: ReactKeyboardEvent<HTMLDivElement>) => {
    switch (e.key) {
      case 'ArrowDown':
        e.preventDefault();
        onMoveActive(1);
        break;
      case 'ArrowUp':
        e.preventDefault();
        onMoveActive(-1);
        break;
      case 'Home':
        e.preventDefault();
        onMoveActive('home');
        break;
      case 'End':
        e.preventDefault();
        onMoveActive('end');
        break;
      case ' ':
        e.preventDefault();
        onToggleActive();
        break;
      case 'Enter': {
        const row = activeIndex >= 0 ? rows[activeIndex] : undefined;
        if (row && row.type === 'dir') {
          e.preventDefault();
          onOpenDir(row.relPath);
        }
        break;
      }
      default:
        break;
    }
  };

  const clickRow = (row: ListRow, e: MouseEvent) => {
    if (row.type === 'note') return;
    onSelect(row.id, e.shiftKey ? 'range' : e.ctrlKey || e.metaKey ? 'toggle' : 'single');
  };

  return (
    <div className="tbl">
      <div className="tbl-row head">
        {HEADERS.map((h) => (
          <div className="cell" key={h}>
            {h}
          </div>
        ))}
      </div>

      <div className="list-wrap" ref={scrollRef} tabIndex={0} onKeyDown={onKeyDown} aria-label="素材列表">
        <div style={{ height: virtualizer.getTotalSize(), position: 'relative', width: '100%' }}>
          {virtualizer.getVirtualItems().map((vi) => {
            const row = rows[vi.index];
            const style = {
              position: 'absolute' as const,
              top: 0,
              left: 0,
              width: '100%',
              height: rowHeight,
              transform: `translateY(${vi.start}px)`,
            };

            if (row.type === 'note') {
              return (
                <div className="tbl-row body" style={style} key={row.id}>
                  <div
                    className="cell tbd"
                    style={{
                      gridColumn: '1 / -1',
                      paddingLeft: `calc(${row.depth} * var(--spacing-3) + var(--spacing-4))`,
                    }}
                  >
                    {row.text}
                  </div>
                </div>
              );
            }

            const selected = selection.has(row.id);
            const indent = `calc(${row.depth} * var(--spacing-3) + var(--spacing-2))`;

            if (row.type === 'dir') {
              const d = row.dir;
              return (
                <div
                  className={[
                    'tbl-row',
                    'body',
                    'is-dir',
                    row.depth === 1 ? 'sub' : '',
                    selected ? 'selected' : '',
                  ]
                    .filter(Boolean)
                    .join(' ')}
                  style={style}
                  key={row.id}
                  draggable
                onDragStart={(e) => {
                  const rels = selection.has(row.id) ? Array.from(selection) : [row.id];
                  onDragStartRows?.(rels.filter((r) => r !== ''));
                  e.dataTransfer.effectAllowed = 'move';
                  e.dataTransfer.setData('text/plain', rels.join('\n'));
                }}
                onClick={(e) => clickRow(row, e)}
                  onDoubleClick={() => onOpenDir(row.relPath)}
                 
                >
                  <div className="cell cell-name" style={{ paddingLeft: indent }}>
                    {row.expandable ? (
                      <button
                        className="caret"
                        type="button"
                        onClick={(e) => {
                          e.stopPropagation();
                          onToggleExpand(row.relPath);
                        }}
                       
                      >
                        {row.expanded ? (
                          <ChevronDown size={13} strokeWidth={2} aria-hidden />
                        ) : (
                          <ChevronRight size={13} strokeWidth={2} aria-hidden />
                        )}
                      </button>
                    ) : (
                      <span className="caret" />
                    )}
                    <Folder className="kind-icon" size={14} strokeWidth={1.75} aria-hidden />
                    <span className="name-text">{d.name}</span>
                  </div>
                  <div className="cell cell-kind">文件夹</div>
                  <div className="cell cell-items">
                    {formatCount(d.directFiles + d.directDirs)} 项 · {formatBytes(d.totalBytes)}
                  </div>
                  <div className="cell cell-prog">
                    <div className="bar">
                      <i style={{ width: '0%' }} />
                    </div>
                    <span className="pct tbd">—</span>
                  </div>
                  <div className="cell cell-time">{formatTime(d.mtimeMs)}</div>
                  <div className="cell cell-status">
                    {d.cloud ? (
                      <span className="badge">
                        <Cloud size={12} strokeWidth={1.75} aria-hidden />
                        云端
                      </span>
                    ) : (
                      <span className="tbd">—</span>
                    )}
                  </div>
                </div>
              );
            }

            const f = row.file;
            const proj = drafts?.get(f.relPath.toLowerCase());
            const displayName = proj?.drafted
              ? proj.path.split(/[\\/]/).filter(Boolean).pop() ?? f.name
              : f.name;
            return (
              <div
                className={[
                  'tbl-row',
                  'body',
                  proj?.drafted ? 'drafted' : '',
                  proj?.removed ? 'removed' : '',
                  selected ? 'selected' : '',
                ]
                  .filter(Boolean)
                  .join(' ')}
                style={style}
                key={row.id}
                onClick={(e) => clickRow(row, e)}
               
              >
                <div className="cell cell-name" style={{ paddingLeft: indent }}>
                  <span className="caret" />
                  <KindIcon kind={f.kind} />
                  <span className="name-text" style={proj?.drafted ? { fontStyle: 'italic' } : undefined}>
                    {displayName}
                  </span>
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
                <div className="cell cell-kind">
                  <KindIcon kind={f.kind} size={13} />
                  {KIND_LABEL[f.kind]}
                </div>
                <div className="cell cell-items">{formatBytes(f.size)}</div>
                <div className="cell cell-prog">
                  <span className="tbd">—</span>
                </div>
                <div className="cell cell-time">{formatTime(f.mtimeMs)}</div>
                <div className="cell cell-status">
                  {proj?.drafted ? (
                    <span className="badge">
                      {proj.removed ? '待删除' : '草稿'}
                    </span>
                  ) : null}
                  {f.cloud ? (
                    <span className="badge">
                      <Cloud size={12} strokeWidth={1.75} aria-hidden />
                      云端
                    </span>
                  ) : null}
                  {!proj?.drafted && !f.cloud ? <span className="tbd">—</span> : null}
                </div>
              </div>
            );
          })}
        </div>
      </div>
    </div>
  );
}
