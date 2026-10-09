/**
 * 左栏（§8.1：工作根 / 目录树 / 外观）。
 *
 * 目录树按需展开（只渲染已展开节点的子目录），不预先铺开整棵树。
 * 每行的「项数」是直接子项数（与展开后看到的行数一致），不是递归统计——
 * 界面上的数字必须能被用户当场验证。
 */

import { ChevronDown, ChevronRight, Folder, FolderOpen, Palette } from 'lucide-react';
import { Fragment } from 'react';

import { subDirsOf, type ScanIndex } from '../lib/folders';
import { formatCount } from '../lib/format';
import { THEME_LABEL, type ThemePref } from '../lib/useTheme';

export interface SidebarProps {
  width: number;
  index: ScanIndex | null;
  root: string | null;
  rootName: string;
  current: string;
  expanded: Set<string>;
  theme: ThemePref;
  onNavigate: (rel: string) => void;
  onToggleExpand: (rel: string) => void;
  onPick: () => void;
  onTheme: (t: ThemePref) => void;
}

interface TreeLevelProps {
  index: ScanIndex;
  rel: string;
  depth: number;
  current: string;
  expanded: Set<string>;
  onNavigate: (rel: string) => void;
  onToggleExpand: (rel: string) => void;
}

function TreeLevel({ index, rel, depth, current, expanded, onNavigate, onToggleExpand }: TreeLevelProps) {
  const dirs = subDirsOf(index, rel);
  return (
    <>
      {dirs.map((d) => {
        const kids = subDirsOf(index, d.relPath);
        const isOpen = expanded.has(d.relPath);
        const active = current === d.relPath;
        return (
          <Fragment key={d.relPath}>
            <div
              className={active ? 'tree-row active' : 'tree-row'}
              style={{ paddingLeft: `calc(${depth} * var(--spacing-3) + var(--spacing-1))` }}
              onClick={() => onNavigate(d.relPath)}
              title={d.relPath}
            >
              {kids.length > 0 ? (
                <button
                  className="tree-caret"
                  type="button"
                  onClick={(e) => {
                    e.stopPropagation();
                    onToggleExpand(d.relPath);
                  }}
                  title={isOpen ? '收起' : '展开'}
                >
                  {isOpen ? (
                    <ChevronDown size={13} strokeWidth={2} aria-hidden />
                  ) : (
                    <ChevronRight size={13} strokeWidth={2} aria-hidden />
                  )}
                </button>
              ) : (
                <span className="tree-caret" />
              )}
              {isOpen || active ? (
                <FolderOpen className="kind-icon" size={14} strokeWidth={1.75} aria-hidden />
              ) : (
                <Folder className="kind-icon" size={14} strokeWidth={1.75} aria-hidden />
              )}
              <span className="tree-name">{d.name}</span>
              <span className="tree-meta">{formatCount(d.directFiles + d.directDirs)}</span>
            </div>
            {isOpen ? (
              <TreeLevel
                index={index}
                rel={d.relPath}
                depth={depth + 1}
                current={current}
                expanded={expanded}
                onNavigate={onNavigate}
                onToggleExpand={onToggleExpand}
              />
            ) : null}
          </Fragment>
        );
      })}
    </>
  );
}

export default function Sidebar({
  width,
  index,
  root,
  rootName,
  current,
  expanded,
  theme,
  onNavigate,
  onToggleExpand,
  onPick,
  onTheme,
}: SidebarProps) {
  const rootOpen = expanded.has('');
  const rootHasKids = index ? subDirsOf(index, '').length > 0 : false;

  return (
    <div className="sidebar" style={{ width }}>
      <div className="side-sec">
        <div className="side-title">
          <span>工作根</span>
          <button className="btn ghost" type="button" onClick={onPick} title="更换素材文件夹">
            更换
          </button>
        </div>
        <div className="workspace-tab" style={{ maxWidth: '100%' }} title={root ?? '尚未选择'}>
          <FolderOpen size={14} strokeWidth={1.75} aria-hidden />
          <span className="path">{root ? rootName : '尚未选择素材文件夹'}</span>
        </div>
      </div>

      <div className="tree">
        {index ? (
          <>
            <div
              className={current === '' ? 'tree-row active' : 'tree-row'}
              style={{ paddingLeft: 'var(--spacing-1)' }}
              onClick={() => onNavigate('')}
              title={index.root}
            >
              {rootHasKids ? (
                <button
                  className="tree-caret"
                  type="button"
                  onClick={(e) => {
                    e.stopPropagation();
                    onToggleExpand('');
                  }}
                  title={rootOpen ? '收起' : '展开'}
                >
                  {rootOpen ? (
                    <ChevronDown size={13} strokeWidth={2} aria-hidden />
                  ) : (
                    <ChevronRight size={13} strokeWidth={2} aria-hidden />
                  )}
                </button>
              ) : (
                <span className="tree-caret" />
              )}
              <FolderOpen className="kind-icon" size={14} strokeWidth={1.75} aria-hidden />
              <span className="tree-name">{rootName}</span>
              <span className="tree-meta">{formatCount(index.childDirs.get('')?.length ?? 0)}</span>
            </div>
            {rootOpen ? (
              <TreeLevel
                index={index}
                rel=""
                depth={1}
                current={current}
                expanded={expanded}
                onNavigate={onNavigate}
                onToggleExpand={onToggleExpand}
              />
            ) : null}
          </>
        ) : (
          <div className="side-sec">
            <span className="tbd">选择素材文件夹后，这里显示目录树。</span>
          </div>
        )}
      </div>

      <div className="side-sec">
        <div className="side-title">
          <span>
            <Palette size={12} strokeWidth={1.75} aria-hidden /> 外观
          </span>
        </div>
        <div className="seg" role="group" aria-label="主题">
          {(Object.keys(THEME_LABEL) as ThemePref[]).map((t) => (
            <button
              key={t}
              className={theme === t ? 'seg-item on' : 'seg-item'}
              type="button"
              onClick={() => onTheme(t)}
            >
              {THEME_LABEL[t]}
            </button>
          ))}
        </div>
        <div className="tbd" style={{ marginTop: 'var(--spacing-2)' }}>
          强调色 <span className="tag-soon">后续阶段</span>
        </div>
        <div className="tbd" style={{ marginTop: 'var(--spacing-1)' }}>
          密度 <span className="tag-soon">后续阶段</span>
        </div>
      </div>
    </div>
  );
}
