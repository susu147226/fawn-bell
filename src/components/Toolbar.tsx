/**
 * 顶栏（§8.1：品牌 / 工作根标签页 / 视图切换 / 搜索 / 面板开关）。
 *
 * 搜索、网格 / 列表 / 详情三档视图都不属于 P0，这里按「已定结构 + 明确标注未接入」
 * 的方式占位：宁可显示禁用与阶段标注，也不做一个假装能用的按钮（§12.4⑨）。
 */

import {
  FolderOpen,
  FolderTree,
  LayoutGrid,
  List,
  PanelLeft,
  PanelRight,
  RefreshCw,
  Search,
  Table,
} from 'lucide-react';

import { ellipsizeMiddle, formatCount } from '../lib/format';
import { THEME_LABEL, type ThemePref } from '../lib/useTheme';
import type { ViewMode } from '../lib/types';

export interface ToolbarProps {
  version: string;
  root: string | null;
  scanning: boolean;
  view: ViewMode;
  leftCollapsed: boolean;
  rightCollapsed: boolean;
  /** 变更集里待提交的条数（设计稿的「提交变更 N」按钮；执行属 P6，这里只显示真数字）。 */
  draftCount: number;
  theme: ThemePref;
  onPick: () => void;
  onRescan: () => void;
  onView: (v: ViewMode) => void;
  onTheme: (t: ThemePref) => void;
  onToggleLeft: () => void;
  onToggleRight: () => void;
}

const VIEW_ICON = { folders: FolderTree, grid: LayoutGrid, list: List, detail: Table };

const VIEW_HINT: Record<ViewMode, string> = {
  folders: '文件夹视图（Ctrl+1）',
  grid: '网格视图',
  list: '列表视图',
  detail: '详情视图',
};

export default function Toolbar({
  version,
  root,
  scanning,
  view,
  leftCollapsed,
  rightCollapsed,
  draftCount,
  theme,
  onPick,
  onRescan,
  onView,
  onTheme,
  onToggleLeft,
  onToggleRight,
}: ToolbarProps) {
  return (
    <div className="titlebar">
      <div className="brand">
        <span className="brand-name">鹿铃</span>
        <span className="brand-version">v{version}</span>
      </div>

      <button
        className={root ? 'workspace-tab' : 'workspace-tab empty'}
        type="button"
        onClick={onPick}
        title={root ?? '选择一个素材文件夹开始'}
      >
        <FolderOpen size={14} strokeWidth={1.75} aria-hidden />
        <span className="path">{root ? ellipsizeMiddle(root, 48) : '未选择素材文件夹'}</span>
      </button>

      <button className="btn" type="button" onClick={onPick} disabled={scanning}>
        <FolderOpen size={14} strokeWidth={1.75} aria-hidden />
        {root ? '更换素材文件夹' : '选择素材文件夹'}
      </button>

      <button className="btn" type="button" onClick={onRescan} disabled={!root || scanning}>
        <RefreshCw size={14} strokeWidth={1.75} aria-hidden />
        重新扫描
      </button>

      <span className="grow" />

      <label className="search" title="搜索素材">
        <Search size={14} strokeWidth={1.75} aria-hidden />
        <input type="search" placeholder="搜索素材" disabled />
      </label>

      <div className="seg" role="group" aria-label="视图">
        {(Object.keys(VIEW_ICON) as ViewMode[]).map((v) => {
          const Icon = VIEW_ICON[v];
          const implemented = v === 'folders' || v === 'grid';
          return (
            <button
              key={v}
              className={view === v ? 'seg-item on' : 'seg-item'}
              type="button"
              onClick={() => onView(v)}
              disabled={!implemented}
              title={VIEW_HINT[v]}
            >
              <Icon size={13} strokeWidth={1.75} aria-hidden />
            </button>
          );
        })}
      </div>

      {/* 设计稿顶栏右侧顺序：搜索 → 视图 → 排序 → 主题 → 提交变更 → 面板开关 */}
      <select className="btn" disabled title="排序" defaultValue="name">
        <option value="name">按名称</option>
      </select>

      <select
        className="btn"
        value={theme}
        onChange={(e) => onTheme(e.target.value as ThemePref)}
        title="主题"
      >
        {(Object.keys(THEME_LABEL) as ThemePref[]).map((t) => (
          <option key={t} value={t}>
            {THEME_LABEL[t]}
          </option>
        ))}
      </select>

      <button
        className="btn primary"
        type="button"
        disabled
        title={
          draftCount > 0
            ? `提交变更 ${draftCount} 项（提交执行属 P6，尚未实现）`
            : '暂无待提交变更（提交执行属 P6，尚未实现）'
        }
      >
        提交变更 <span className="status-num">{formatCount(draftCount)}</span>
      </button>

      <button
        className="btn ghost icon"
        type="button"
        onClick={onToggleLeft}
        title={leftCollapsed ? '显示左栏' : '收起左栏'}
      >
        <PanelLeft size={14} strokeWidth={1.75} aria-hidden />
      </button>
      <button
        className="btn ghost icon"
        type="button"
        onClick={onToggleRight}
        title={rightCollapsed ? '显示右栏' : '收起右栏'}
      >
        <PanelRight size={14} strokeWidth={1.75} aria-hidden />
      </button>
    </div>
  );
}
