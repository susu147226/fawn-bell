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
import { KIND_LABEL } from '../lib/types';

export interface AssetListProps {
  rows: ListRow[];
  selection: Set<string>;
  activeId: string | null;
  onSelect: (id: string, mode: 'single' | 'toggle' | 'range') => void;
  onOpenDir: (relPath: string) => void;
  onToggleExpand: (relPath: string) => void;
  onMoveActive: (delta: number | 'home' | 'end') => void;
  onToggleActive: () => void;
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
  onSelect,
  onOpenDir,
  onToggleExpand,
  onMoveActive,
  onToggleActive,
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
          <div className="cell" key={h} title={h === '整理进度' ? '整理进度：P2 接入' : undefined}>
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
                  onClick={(e) => clickRow(row, e)}
                  onDoubleClick={() => onOpenDir(row.relPath)}
                  title={d.relPath}
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
                        title={row.expanded ? '收起一层' : '展开一层'}
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
                  <div className="cell cell-prog" title="整理进度：P2 接入">
                    <div className="bar">
                      <i style={{ width: '0%' }} />
                    </div>
                    <span className="pct tbd">—</span>
                  </div>
                  <div className="cell cell-time">{formatTime(d.mtimeMs)}</div>
                  <div className="cell cell-status">
                    {d.cloud ? (
                      <span className="badge" title="云端占位文件">
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
            return (
              <div
                className={['tbl-row', 'body', selected ? 'selected' : ''].filter(Boolean).join(' ')}
                style={style}
                key={row.id}
                onClick={(e) => clickRow(row, e)}
                title={f.relPath}
              >
                <div className="cell cell-name" style={{ paddingLeft: indent }}>
                  <span className="caret" />
                  <KindIcon kind={f.kind} />
                  <span className="name-text">{f.name}</span>
                </div>
                <div className="cell cell-kind">
                  <KindIcon kind={f.kind} size={13} />
                  {KIND_LABEL[f.kind]}
                </div>
                <div className="cell cell-items">{formatBytes(f.size)}</div>
                <div className="cell cell-prog" title="整理进度：P2 接入">
                  <span className="tbd">—</span>
                </div>
                <div className="cell cell-time">{formatTime(f.mtimeMs)}</div>
                <div className="cell cell-status">
                  {f.cloud ? (
                    <span className="badge" title="云端占位文件">
                      <Cloud size={12} strokeWidth={1.75} aria-hidden />
                      云端
                    </span>
                  ) : (
                    <span className="tbd">—</span>
                  )}
                </div>
              </div>
            );
          })}
        </div>
      </div>
    </div>
  );
}
