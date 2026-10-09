/**
 * 可拖拽窗格宽度（§8.1 三栏可拖拽、可折叠；宽度记在本地，下次打开保持）。
 *
 * 界面只以指针拖拽调整布局，不落任何业务数据；localStorage 在隐私模式下不可用时
 * 静默退回默认宽度（§14 素材树零写入与配置文件写入无关，但仍不因存储失败打断使用）。
 */

import { useCallback, useEffect, useRef, useState, type PointerEvent as ReactPointerEvent } from 'react';

function clamp(v: number, min: number, max: number): number {
  return Math.min(max, Math.max(min, v));
}

export interface ResizableOptions {
  /** localStorage 键，按窗格区分。 */
  storageKey: string;
  /** 默认宽度（px）。 */
  initial: number;
  min: number;
  max: number;
  /** `left`：窗格在左边，向右拖变宽；`right`：窗格在右边，向左拖变宽。 */
  side: 'left' | 'right';
}

export interface Resizable {
  width: number;
  dragging: boolean;
  onPointerDown: (e: ReactPointerEvent<HTMLDivElement>) => void;
}

export function useResizable({ storageKey, initial, min, max, side }: ResizableOptions): Resizable {
  const [width, setWidth] = useState(() => {
    try {
      const raw = window.localStorage.getItem(storageKey);
      if (raw !== null) {
        const v = Number(raw);
        if (Number.isFinite(v)) return clamp(v, min, max);
      }
    } catch {
      /* 存储不可用：用默认宽度 */
    }
    return initial;
  });
  const [dragging, setDragging] = useState(false);
  const stop = useRef<(() => void) | null>(null);

  useEffect(() => {
    try {
      window.localStorage.setItem(storageKey, String(Math.round(width)));
    } catch {
      /* 忽略 */
    }
  }, [storageKey, width]);

  useEffect(() => () => stop.current?.(), []);

  const onPointerDown = useCallback(
    (e: ReactPointerEvent<HTMLDivElement>) => {
      if (e.button !== 0) return;
      e.preventDefault();
      const startX = e.clientX;
      const startW = width;
      setDragging(true);

      const move = (ev: PointerEvent) => {
        const delta = ev.clientX - startX;
        setWidth(clamp(side === 'left' ? startW + delta : startW - delta, min, max));
      };
      const up = () => {
        setDragging(false);
        window.removeEventListener('pointermove', move);
        window.removeEventListener('pointerup', up);
        stop.current = null;
      };
      window.addEventListener('pointermove', move);
      window.addEventListener('pointerup', up);
      stop.current = up;
    },
    [width, min, max, side],
  );

  return { width, dragging, onPointerDown };
}
