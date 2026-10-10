/**
 * 左栏（§8.1：工作根 / 目录树 / 外观）。
 *
 * 目录树按需展开（只渲染已展开节点的子目录），不预先铺开整棵树。
 * 每行的「项数」是直接子项数（与展开后看到的行数一致），不是递归统计——
 * 界面上的数字必须能被用户当场验证。
 */

import { ChevronDown, ChevronRight, Folder, FolderOpen, Lock, Palette } from 'lucide-react';
import { Fragment, useEffect, useState } from 'react';

import { subDirsOf, type ScanIndex } from '../lib/folders';
import { formatCount } from '../lib/format';
import { api, errorText } from '../lib/ipc';
import type { Group, ProtectionStats } from '../lib/types';
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
  /** 外层在保护区 / 分组变化后自增，用来触发左栏重新读数（§7.5 / §7.6）。 */
  refreshToken?: number;
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
  refreshToken,
}: SidebarProps) {
  // §7.5 / §7.6：分组、智能集合、保护区都读库里的真数据（智能集合动态求值）
  const [groups, setGroups] = useState<Group[] | null>(null);
  const [stats, setStats] = useState<ProtectionStats | null>(null);
  const [adding, setAdding] = useState(false);
  const [newName, setNewName] = useState('');
  const [err, setErr] = useState<string | null>(null);

  const reload = async () => {
    try {
      const [g, s] = await Promise.all([api.groupsList(), api.protectionStats()]);
      // 智能集合除了「重复内容」（由去重用例物化）之外都要现算，显示真数字
      const withCounts = await Promise.all(
        g.map(async (x) =>
          x.kind === 'smart' && !(x.ruleJson ?? '').includes('duplicates')
            ? { ...x, memberCount: (await api.groupMembersEval(x.id)).length }
            : x,
        ),
      );
      setGroups(withCounts);
      setStats(s);
      setErr(null);
    } catch (e) {
      setErr(errorText(e));
    }
  };

  useEffect(() => {
    void reload();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [refreshToken]);

  const createGroup = async () => {
    const name = newName.trim();
    if (!name) return;
    try {
      await api.groupCreate(name);
      setNewName('');
      setAdding(false);
      await reload();
    } catch (e) {
      setErr(errorText(e));
    }
  };

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

      {/* §7.5 分组 + 智能集合：读库里的真数据；智能集合每次打开动态求值，
          「重复内容」用的是去重用例物化进 asset_group 的成员（同源，§16⑰①）。 */}
      <div className="side-sec">
        <div className="side-title">
          <span>分组</span>
          <button className="btn ghost" type="button" onClick={() => setAdding((v) => !v)} title="新建分组">
            +
          </button>
        </div>
        {adding ? (
          <div style={{ display: 'flex', gap: 'var(--spacing-1)' }}>
            <input
              type="text"
              value={newName}
              placeholder="分组名"
              onChange={(e) => setNewName(e.target.value)}
              onKeyDown={(e) => {
                if (e.key === 'Enter') void createGroup();
              }}
              style={{ width: '100%' }}
            />
            <button className="btn" type="button" disabled={!newName.trim()} onClick={() => void createGroup()}>
              建立
            </button>
          </div>
        ) : null}
        {(groups ?? []).filter((g) => g.kind !== 'smart').length === 0 ? (
          <div className="tbd">暂无分组（点 + 新建）</div>
        ) : (
          (groups ?? [])
            .filter((g) => g.kind !== 'smart')
            .map((g) => (
              <div className="tree-row" key={g.id} title={`${g.name} · ${formatCount(g.memberCount)} 项`}>
                <span className="tree-name">{g.name}</span>
                <span className="tree-meta">{formatCount(g.memberCount)}</span>
              </div>
            ))
        )}
      </div>

      <div className="side-sec">
        <div className="side-title">
          <span>智能集合</span>
          <span className="tag-soon">动态</span>
        </div>
        {(groups ?? [])
          .filter((g) => g.kind === 'smart')
          .map((g) => (
            <div
              className="tree-row"
              key={g.id}
              title={`${g.name} · ${formatCount(g.memberCount)} 项（每次打开动态求值）`}
            >
              <span className="tree-name">{g.name}</span>
              <span className="tree-meta">{formatCount(g.memberCount)}</span>
            </div>
          ))}
      </div>

      {/* §7.6 保护区：横切安全属性，独立于分组；总条目数与「当周新增」并列显示 */}
      <div className="side-sec">
        <div className="side-title">
          <span>
            <Lock size={12} strokeWidth={1.75} aria-hidden /> 保护区
          </span>
          <span className="tag-soon">锁定</span>
        </div>
        <div className="tbd">
          共 {formatCount(stats?.total ?? 0)} 项 · 当周新增 {formatCount(stats?.weekNew ?? 0)}
        </div>
        <div className="tbd">受保护项在批量操作中被默认跳过</div>
      </div>

      {err ? (
        <div className="side-sec">
          <span className="tbd">{err}</span>
        </div>
      ) : null}

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
