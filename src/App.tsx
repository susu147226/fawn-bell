/**
 * 鹿铃 · 应用外壳（P0）
 *
 * P0 交付的是「能看到素材」：选一个素材文件夹 → 只读扫描（可取消、有进度与预估）→
 * 三栏界面（目录树 / 混排列表（虚拟滚动）/ 详情）→ 状态条。
 *
 * 边界（执行版 §14）：
 *   - 只读：全程没有任何写文件、改名、移动、删除的调用；扫描结果只存在内存里（P0 不落库）。
 *   - 零网络：界面的所有数据都来自本地后端命令与事件。
 *   - 不做的事都有明确标注，不显示假数据（§12.4⑨）。
 */

import { getCurrentWindow } from '@tauri-apps/api/window';
import { FolderOpen, ScanLine } from 'lucide-react';
import { useCallback, useEffect, useMemo, useState, type ReactNode } from 'react';

import AssetList from './components/AssetList';
import Breadcrumb from './components/Breadcrumb';
import CloseDialog from './components/CloseDialog';
import DetailPanel, { type PanelKind } from './components/DetailPanel';
import EmptyState from './components/EmptyState';
import Notice, { type NoticeKind } from './components/Notice';
import ScanBanner from './components/ScanBanner';
import Sidebar from './components/Sidebar';
import StatusBar from './components/StatusBar';
import Toolbar from './components/Toolbar';
import TypeStats from './components/TypeStats';
import { breadcrumbOf, buildIndex, dirDisplayName } from './lib/folders';
import { basename, formatCount } from './lib/format';
import { api, errorText, onDraftChanged, onScanDone, onScanProgress, pickFolder } from './lib/ipc';
import { buildRows } from './lib/rows';
import { useResizable } from './lib/useResizable';
import { useTheme } from './lib/useTheme';
import type {
  AppInfo,
  DraftList,
  Projection,
  ScanDone,
  ScanProgress,
  ScanResult,
  ViewMode,
} from './lib/types';

interface NoticeState {
  kind: NoticeKind;
  title: string;
  lines?: string[];
}

export default function App() {
  const theme = useTheme();

  const [info, setInfo] = useState<AppInfo | null>(null);
  const [root, setRoot] = useState<string | null>(null);
  const [result, setResult] = useState<ScanResult | null>(null);
  const [progress, setProgress] = useState<ScanProgress | null>(null);
  const [scanId, setScanId] = useState<number | null>(null);
  const [notice, setNotice] = useState<NoticeState | null>(null);

  const [current, setCurrent] = useState('');
  const [expanded, setExpanded] = useState<Set<string>>(() => new Set(['']));
  const [selected, setSelected] = useState<Set<string>>(() => new Set());
  const [activeId, setActiveId] = useState<string | null>(null);
  const [rangeAnchor, setRangeAnchor] = useState<string | null>(null);
  const [view, setView] = useState<ViewMode>('folders');
  const [leftCollapsed, setLeftCollapsed] = useState(false);
  const [rightCollapsed, setRightCollapsed] = useState(false);
  /** 右栏当前显示的 P1 工具面板（去重集合 / 重定位向导）。 */
  const [panel, setPanel] = useState<PanelKind>('none');
  /** 草稿（§7.2）：列表 + 投影（投影键＝根内相对路径的小写形式）。 */
  const [draftList, setDraftList] = useState<DraftList | null>(null);
  const [draftMap, setDraftMap] = useState<Map<string, Projection>>(new Map());
  /** 关闭前询问（§7.2：变更集非空时必须三选项并列）。 */
  const [closeAsk, setCloseAsk] = useState(false);

  const left = useResizable({ storageKey: 'luling.leftWidth', initial: 248, min: 180, max: 420, side: 'left' });
  const right = useResizable({ storageKey: 'luling.rightWidth', initial: 320, min: 240, max: 480, side: 'right' });

  const index = useMemo(() => (result ? buildIndex(result) : null), [result]);
  const summary = result?.summary ?? null;

  /* ── 启动：应用信息 + 未完成扫描的恢复 ─────────────────────────── */

  useEffect(() => {
    let alive = true;
    api
      .appInfo()
      .then((i) => {
        if (alive) setInfo(i);
      })
      .catch(() => {
        /* 拿不到版本信息不影响使用 */
      });
    api
      .scanSnapshot()
      .then((s) => {
        if (!alive || s.running === null) return;
        setScanId(s.running);
        if (s.progress) setProgress(s.progress);
        setNotice({ kind: 'info', title: '正在扫描', lines: ['界面重新载入前的那次扫描还在进行，完成后会显示结果。'] });
      })
      .catch(() => {
        /* 忽略 */
      });
    return () => {
      alive = false;
    };
  }, []);

  /* ── 扫描 ─────────────────────────────────────────────────────── */

  const handleDone = useCallback(async (d: ScanDone) => {
    setScanId(null);
    setProgress(null);

    if (d.status === 'failed') {
      setNotice({ kind: 'error', title: '扫描失败', lines: d.message ? [d.message] : undefined });
      return;
    }
    if (d.status === 'cancelled') {
      setNotice({ kind: 'warn', title: '扫描已取消', lines: ['这次扫描的结果已丢弃；素材文件夹没有被改动。'] });
      return;
    }

    try {
      const r = await api.scanResult(d.scanId);
      setResult(r);
      setRoot(r.summary.root);
      setCurrent('');
      setExpanded(new Set(['']));
      setSelected(new Set());
      setActiveId(null);
      setRangeAnchor(null);

      const cloud = r.files.filter((f) => f.cloud).length;
      const lines: string[] = [];
      if (r.summary.truncated) {
        lines.push(
          `文件数超过本次上限（${formatCount(r.summary.fileCount)} 个），列表被截断；命令行可以用 --limit 提高上限。`,
        );
      }
      if (r.summary.warnings.length > 0) {
        lines.push(`有 ${formatCount(r.summary.warnings.length)} 处目录无法读取（例如权限不足），已跳过。`);
      }
      if (r.summary.bigDirs.length > 0) {
        lines.push(`有 ${formatCount(r.summary.bigDirs.length)} 个文件夹的直接子项超过 5000 个，展开会比较慢。`);
      }
      if (cloud > 0) {
        lines.push(`有 ${formatCount(cloud)} 个云端占位文件：磁盘上只有占位信息，读取内容时可能触发下载。`);
      }
      if (r.summary.excluded.length > 0) {
        lines.push(`已自动排除鹿铃自己的库目录：${r.summary.excluded.join('、')}`);
      }
      // P1：索引结果（增量扫描与内容指纹的可见凭据）
      if (d.index) {
        lines.push(
          `索引：新增 ${formatCount(d.index.inserted)} · 更新 ${formatCount(d.index.updated)} · 未变 ${formatCount(
            d.index.unchanged,
          )} · 缺失 ${formatCount(d.index.missing)}｜库内共 ${formatCount(d.index.entries)} 项`,
        );
        if (d.index.refs > 0) {
          lines.push(`已记录 ${formatCount(d.index.refs)} 行文件名引用，后续改名时可提示「该文件被 N 处引用」。`);
        }
        for (const w of d.index.warnings.slice(0, 3)) lines.push(w);
      } else if (d.indexError) {
        lines.push(d.indexError);
      }
      setNotice(
        lines.length > 0
          ? {
              kind: r.summary.truncated || d.indexError ? 'warn' : 'info',
              title: '扫描完成，有几点需要知道',
              lines,
            }
          : null,
      );
    } catch (e) {
      setNotice({ kind: 'error', title: '读取扫描结果失败', lines: [errorText(e)] });
    }
  }, []);

  useEffect(() => {
    const pProgress = onScanProgress((p) => setProgress(p));
    const pDone = onScanDone((d) => {
      void handleDone(d);
    });
    return () => {
      void pProgress.then((f) => f());
      void pDone.then((f) => f());
    };
  }, [handleDone]);

  const startScan = useCallback(async (dir: string) => {
    setNotice(null);
    setResult(null);
    setProgress(null);
    setCurrent('');
    setExpanded(new Set(['']));
    setSelected(new Set());
    setActiveId(null);
    setRangeAnchor(null);
    setRoot(dir);
    try {
      const id = await api.scanStart(dir);
      setScanId(id);
    } catch (e) {
      setScanId(null);
      setNotice({ kind: 'error', title: '无法扫描这个文件夹', lines: [errorText(e)] });
    }
  }, []);

  const onPick = useCallback(async () => {
    try {
      const dir = await pickFolder();
      if (dir) await startScan(dir);
    } catch (e) {
      setNotice({ kind: 'error', title: '打开「选择文件夹」失败', lines: [errorText(e)] });
    }
  }, [startScan]);

  const onRescan = useCallback(() => {
    if (root) void startScan(root);
  }, [root, startScan]);

  const onCancelScan = useCallback(() => {
    void api.scanCancel().catch(() => {
      /* 取消失败不打扰用户：扫描自己会走完 */
    });
  }, []);

  /* ── 列表行与选择 ─────────────────────────────────────────────── */

  const rows = useMemo(() => (index ? buildRows(index, current, expanded) : []), [index, current, expanded]);
  const selectableIds = useMemo(
    () => rows.filter((r) => r.type !== 'note').map((r) => r.id),
    [rows],
  );

  const onNavigate = useCallback((rel: string) => {
    setCurrent(rel);
    setSelected(new Set());
    setActiveId(null);
    setRangeAnchor(null);
  }, []);

  const onToggleExpand = useCallback((rel: string) => {
    setExpanded((prev) => {
      const next = new Set(prev);
      if (next.has(rel)) next.delete(rel);
      else next.add(rel);
      return next;
    });
  }, []);

  const onSelect = useCallback(
    (id: string, mode: 'single' | 'toggle' | 'range') => {
      setActiveId(id);
      if (mode === 'single') {
        setRangeAnchor(id);
        setSelected(new Set([id]));
        return;
      }
      if (mode === 'toggle') {
        setRangeAnchor(id);
        setSelected((prev) => {
          const next = new Set(prev);
          if (next.has(id)) next.delete(id);
          else next.add(id);
          return next;
        });
        return;
      }
      const anchor = rangeAnchor ?? id;
      const from = selectableIds.indexOf(anchor);
      const to = selectableIds.indexOf(id);
      if (from < 0 || to < 0) {
        setSelected(new Set([id]));
        return;
      }
      const [a, b] = from <= to ? [from, to] : [to, from];
      setSelected(new Set(selectableIds.slice(a, b + 1)));
    },
    [rangeAnchor, selectableIds],
  );

  const onMoveActive = useCallback(
    (delta: number | 'home' | 'end') => {
      if (selectableIds.length === 0) return;
      const cur = activeId ? selectableIds.indexOf(activeId) : -1;
      let next: number;
      if (delta === 'home') next = 0;
      else if (delta === 'end') next = selectableIds.length - 1;
      else next = cur < 0 ? 0 : Math.min(selectableIds.length - 1, Math.max(0, cur + delta));
      const id = selectableIds[next];
      setActiveId(id);
      setSelected(new Set([id]));
      setRangeAnchor(id);
    },
    [activeId, selectableIds],
  );

  const onToggleActive = useCallback(() => {
    if (!activeId) return;
    setSelected((prev) => {
      const next = new Set(prev);
      if (next.has(activeId)) next.delete(activeId);
      else next.add(activeId);
      return next;
    });
  }, [activeId]);

  /* ── 快捷键（§8.4 / §7.2：Esc 取消扫描 · Ctrl+1 文件夹视图 · Ctrl+Z/Y 撤销重做草稿） ── */

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        if (scanId !== null) onCancelScan();
        else setSelected(new Set());
        return;
      }
      if ((e.ctrlKey || e.metaKey) && e.key === '1') {
        e.preventDefault();
        setView('folders');
        return;
      }
      // §7.2：撤销 / 重做只在草稿上下文内动，**不触碰磁盘**
      if ((e.ctrlKey || e.metaKey) && (e.key === 'z' || e.key === 'Z')) {
        e.preventDefault();
        api
          .draftUndo()
          .then((list) =>
            setNotice({
              kind: 'info',
              title: '已撤销一条草稿',
              lines: [`当前待提交 ${formatCount(list.count)} 项，其中 ${formatCount(list.problems)} 项有问题。`],
            }),
          )
          .catch((err) => setNotice({ kind: 'error', title: '撤销失败', lines: [errorText(err)] }));
        return;
      }
      if ((e.ctrlKey || e.metaKey) && (e.key === 'y' || e.key === 'Y')) {
        e.preventDefault();
        api
          .draftRedo()
          .then((list) =>
            setNotice({
              kind: 'info',
              title: '已重做一条草稿',
              lines: [`当前待提交 ${formatCount(list.count)} 项，其中 ${formatCount(list.problems)} 项有问题。`],
            }),
          )
          .catch((err) => setNotice({ kind: 'error', title: '重做失败', lines: [errorText(err)] }));
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [scanId, onCancelScan]);

  /* ── 渲染 ─────────────────────────────────────────────────────── */

  const rootName = index ? dirDisplayName(index, '') : root ? basename(root) : '未选择';
  const selectedList = useMemo(() => Array.from(selected).sort(), [selected]);
  const currentKinds = index?.dirByRel.get(current)?.kindCounts ?? [];

  /* ── 草稿（§7.2）：拉取 + 订阅变化 ── */
  useEffect(() => {
    let alive = true;
    api
      .draftList()
      .then((d) => {
        if (alive) setDraftList(d);
      })
      .catch(() => {
        /* 拿不到草稿不影响使用 */
      });
    const un = onDraftChanged((d) => setDraftList(d));
    return () => {
      alive = false;
      void un.then((f) => f());
    };
  }, []);

  /* 投影：把当前列表里看得见的路径交给后端折算成「界面该显示的样子」（§7.2 投影视图） */
  useEffect(() => {
    if (!root || rows.length === 0) {
      setDraftMap(new Map());
      return;
    }
    const visible = rows.filter((r) => r.type !== 'note');
    const paths = visible.map((r) => `${root}\\${r.relPath.replace(/\//g, '\\')}`);
    let alive = true;
    api
      .draftProject(paths)
      .then((ps) => {
        if (!alive) return;
        const m = new Map<string, Projection>();
        visible.forEach((r, i) => {
          const p = ps[i];
          if (p && (p.drafted || p.removed)) m.set(r.relPath.toLowerCase(), p);
        });
        setDraftMap(m);
      })
      .catch(() => setDraftMap(new Map()));
    return () => {
      alive = false;
    };
  }, [draftList, rows, root]);

  /* ── 关闭窗口（§7.2）：变更集非空时先问，三个选项并列、不设默认焦点 ── */
  useEffect(() => {
    let unlisten: (() => void) | undefined;
    let alive = true;
    void getCurrentWindow()
      .onCloseRequested((event) => {
        if ((draftList?.count ?? 0) > 0) {
          event.preventDefault();
          setCloseAsk(true);
        }
      })
      .then((f) => {
        if (alive) unlisten = f;
        else f();
      });
    return () => {
      alive = false;
      unlisten?.();
    };
  }, [draftList]);

  const closeWindow = useCallback(() => {
    void getCurrentWindow().close();
  }, []);

  const discardAndClose = useCallback(() => {
    void api
      .draftClear()
      .catch(() => undefined)
      .then(() => closeWindow());
  }, [closeWindow]);

  /** 状态条上的撤销/重做按钮：与 Ctrl+Z / Ctrl+Y 走同一条链路（只动草稿，不碰磁盘）。 */
  const undoDraft = useCallback(() => {
    void api
      .draftUndo()
      .then(setDraftList)
      .catch((e) => setNotice({ kind: 'error', title: '撤销失败', lines: [errorText(e)] }));
  }, []);

  const redoDraft = useCallback(() => {
    void api
      .draftRedo()
      .then(setDraftList)
      .catch((e) => setNotice({ kind: 'error', title: '重做失败', lines: [errorText(e)] }));
  }, []);

  let mainBody: ReactNode;
  if (index && summary) {
    mainBody =
      rows.length === 0 ? (
        <EmptyState
          art={<FolderOpen size={40} strokeWidth={1.25} />}
          title="空文件夹"
          desc="这个文件夹里还没有素材。把素材放进来之后重新扫描即可。"
          actions={
            <button className="btn" type="button" onClick={onRescan}>
              重新扫描
            </button>
          }
        />
      ) : (
        <>
          {/* 设计稿中栏：视图行 + 状态 chips（草稿用变更集真值，其余按阶段标注） */}
          <div
            style={{
              display: 'flex',
              alignItems: 'center',
              gap: 'var(--spacing-2)',
              padding: 'var(--spacing-1) var(--spacing-3)',
              borderBottom: '1px solid var(--border)',
            }}
          >
            <span className="tbd">文件夹视图 · 共 {formatCount(rows.length)} 项</span>
            <span className="grow" />
            <span className="badge" title="草稿态条目（取自变更集真值）">
              草稿 {formatCount(draftList?.count ?? 0)}
            </span>
            <span className="badge" title="已整理：P6 接入后由已整理表提供">
              已整理 <span className="tag-soon">P6</span>
            </span>
            <span className="badge" title="受保护：P4 接入">
              受保护 <span className="tag-soon">P4</span>
            </span>
          </div>
          <div className="seg" role="group" aria-label="筛选" style={{ margin: 'var(--spacing-1) var(--spacing-3)' }}>
            <button className="seg-item on" type="button" disabled>
              全部 {formatCount(rows.length)}
            </button>
            <button className="seg-item" type="button" disabled title="按类型筛选（P5 接入）">
              类型筛选<span className="tag-soon">P5</span>
            </button>
            <button className="seg-item" type="button" disabled title="按大小区间筛选（P5 接入）">
              大小范围<span className="tag-soon">P5</span>
            </button>
          </div>
          <AssetList
            rows={rows}
            selection={selected}
            activeId={activeId}
            drafts={draftMap}
            onSelect={onSelect}
            onOpenDir={onNavigate}
            onToggleExpand={onToggleExpand}
            onMoveActive={onMoveActive}
            onToggleActive={onToggleActive}
          />
        </>
      );
  } else if (scanId !== null) {
    mainBody = (
      <EmptyState
        art={<ScanLine size={40} strokeWidth={1.25} />}
        title="正在读取文件夹…"
        desc="只读取文件名、类型、大小与时间；不会移动、改名或删除任何文件。"
        actions={
          <button className="btn" type="button" onClick={onCancelScan}>
            取消扫描
          </button>
        }
      />
    );
  } else {
    mainBody = (
      <EmptyState
        art={<FolderOpen size={40} strokeWidth={1.25} />}
        title="还没有选择素材文件夹"
        desc="鹿铃只读取你选择的文件夹，不会改动里面的任何文件。选一个文件夹开始吧。"
        actions={
          <button className="btn primary" type="button" onClick={onPick}>
            <FolderOpen size={14} strokeWidth={1.75} aria-hidden />
            选择素材文件夹
          </button>
        }
      />
    );
  }

  return (
    <div className="app">
      <Toolbar
        version={info?.version ?? '0.1.0'}
        root={root}
        scanning={scanId !== null}
        view={view}
        leftCollapsed={leftCollapsed}
        rightCollapsed={rightCollapsed}
        draftCount={draftList?.count ?? 0}
        theme={theme.pref}
        onPick={onPick}
        onRescan={onRescan}
        onView={setView}
        onTheme={theme.setPref}
        onToggleLeft={() => setLeftCollapsed((v) => !v)}
        onToggleRight={() => setRightCollapsed((v) => !v)}
      />

      <div className="body">
        {leftCollapsed ? null : (
          <>
            <Sidebar
              width={left.width}
              index={index}
              root={root}
              rootName={rootName}
              current={current}
              expanded={expanded}
              theme={theme.pref}
              onNavigate={onNavigate}
              onToggleExpand={onToggleExpand}
              onPick={onPick}
              onTheme={theme.setPref}
            />
            <div
              className={left.dragging ? 'splitter dragging' : 'splitter'}
              onPointerDown={left.onPointerDown}
              role="separator"
              aria-orientation="vertical"
            />
          </>
        )}

        <div className="main">
          {index && summary ? (
            <>
              <Breadcrumb
                rootName={rootName}
                crumbs={breadcrumbOf(current)}
                current={current}
                onNavigate={onNavigate}
              />
              <TypeStats byKind={currentKinds} />
            </>
          ) : null}

          {progress ? <ScanBanner progress={progress} onCancel={onCancelScan} /> : null}
          {notice ? <Notice kind={notice.kind} title={notice.title} lines={notice.lines} /> : null}

          {mainBody}
        </div>

        {rightCollapsed ? null : (
          <>
            <div
              className={right.dragging ? 'splitter dragging' : 'splitter'}
              onPointerDown={right.onPointerDown}
              role="separator"
              aria-orientation="vertical"
            />
            <DetailPanel
              width={right.width}
              index={index}
              current={current}
              selected={selectedList}
              summary={summary}
              panel={panel}
              onPanel={setPanel}
              onNotice={setNotice}
            />
          </>
        )}
      </div>

      <StatusBar
        selectedCount={selected.size}
        summary={summary}
        scanning={scanId !== null}
        draftCount={draftList?.count ?? 0}
        draftProblems={draftList?.problems ?? 0}
        canRedo={(draftList?.redo ?? 0) > 0}
        onUndo={undoDraft}
        onRedo={redoDraft}
      />

      {closeAsk ? (
        <CloseDialog
          count={draftList?.count ?? 0}
          onSaveAndClose={closeWindow}
          onDiscardAndClose={discardAndClose}
          onCancel={() => setCloseAsk(false)}
        />
      ) : null}
    </div>
  );
}
