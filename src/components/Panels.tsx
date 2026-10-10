/**
 * 右栏里的两个 P1 界面入口：
 * - 「重复内容」（§7.12）：判定与「重复内容」智能集合同源，界面只展示后端给的结论与证据；
 * - 「重新定位素材树」向导（§13.4）：三档匹配 → 逐条看依据 → 按档位应用。
 *
 * 两条铁律：不显示假数据（没有就说没有）；任何改写索引的动作都要先出计划、再由用户按下按钮。
 */

import { useEffect, useState } from 'react';
import { FolderSearch, ListChecks } from 'lucide-react';

import { formatBytes, formatCount, formatDateTime } from '../lib/format';
import { api, errorText, pickFolder } from '../lib/ipc';
import type { DedupeReport, DraftList, LibraryInfo, RelocatePlan, SkippedProtected } from '../lib/types';

export interface NoticePayload {
  kind: 'info' | 'warn' | 'error';
  title: string;
  lines?: string[];
}

const POLICY_LABEL: Record<string, string> = {
  earliestCreated: '保留最早创建（默认）',
  shortestPath: '保留路径最短',
  manual: '手工决定（不替你选）',
};

const CONFIDENCE_LABEL: Record<string, string> = {
  high: '高置信',
  needsConfirm: '待确认',
  unmatched: '无法匹配',
};

function Back({ onClose }: { onClose: () => void }) {
  return (
    <button className="btn" type="button" onClick={onClose}>
      返回
    </button>
  );
}

/* ── 重复内容 ─────────────────────────────────────────────────────── */

export function DedupePanel({ onClose }: { onClose: () => void }) {
  const [policy, setPolicy] = useState('earliestCreated');
  const [report, setReport] = useState<DedupeReport | null>(null);
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState<string | null>(null);

  const run = async (p: string) => {
    setBusy(true);
    setErr(null);
    try {
      setReport(await api.dedupeReport(p));
    } catch (e) {
      setErr(errorText(e));
    } finally {
      setBusy(false);
    }
  };

  useEffect(() => {
    void run(policy);
    // 只在打开面板时自动跑一次；换策略由下拉框自己触发
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  return (
    <>
      <div className="detail-sec">
        <div className="detail-title">
          <ListChecks size={14} strokeWidth={1.75} aria-hidden /> 重复内容（内容去重）
        </div>
        <div className="kv">
          <div className="kv-key">保留策略</div>
          <div className="kv-val">
            <select
              value={policy}
              onChange={(e) => {
                setPolicy(e.target.value);
                void run(e.target.value);
              }}
            >
              {Object.entries(POLICY_LABEL).map(([k, v]) => (
                <option key={k} value={k}>
                  {v}
                </option>
              ))}
            </select>{' '}
            <button className="btn" type="button" disabled={busy} onClick={() => void run(policy)}>
              {busy ? '检测中…' : '重新检测'}
            </button>{' '}
            <Back onClose={onClose} />
          </div>
        </div>
      </div>

      {err ? (
        <div className="detail-sec">
          <div className="tbd">检测失败：{err}</div>
        </div>
      ) : null}

      {report ? (
        <>
          <div className="detail-sec">
            <div className="detail-title">结论</div>
            <div className="kv">
              <div className="kv-key">候选 / 重复组</div>
              <div className="kv-val">
                {formatCount(report.candidates)} 项 / {formatCount(report.groupCount)} 组
              </div>
              <div className="kv-key">重复项 / 保留项</div>
              <div className="kv-val">
                {formatCount(report.duplicateCount)} 项 / {formatCount(report.keepers)} 项
              </div>
              <div className="kv-key">可清理</div>
              <div className="kv-val">{formatBytes(Math.max(0, report.wasteBytes))}</div>
              <div className="kv-key">判定依据</div>
              <div className="kv-val mono">体积 + 首尾各 64 KB 分段哈希，再逐字节比对全长</div>
            </div>
          </div>

          {report.groups.length === 0 ? (
            <div className="detail-sec">
              <div className="tbd">无重复内容</div>
            </div>
          ) : (
            <div className="detail-sec">
              <div className="detail-title">重复组（前 {Math.min(report.groups.length, 20)} 组）</div>
              {report.groups.slice(0, 20).map((g, i) => (
                <div key={`${g.hashPartial}-${i}`} style={{ marginBottom: 'var(--spacing-3)' }}>
                  <div className="kv-key">
                    组 {i + 1} · {formatBytes(g.size)} · <span className="mono">{g.hashPartial}</span>
                  </div>
                  {g.members.map((m) => (
                    <div className="kv-val mono" key={m.assetId}>
                      {m.keeper ? '【保留】' : '【重复】'}
                      {m.absPath ?? m.relPath}
                    </div>
                  ))}
                </div>
              ))}
            </div>
          )}

          {report.warnings.length > 0 ? (
            <div className="detail-sec">
              <div className="detail-title">提醒</div>
              {report.warnings.slice(0, 6).map((w) => (
                <div className="tbd" key={w}>
                  {w}
                </div>
              ))}
            </div>
          ) : null}

          <div className="detail-sec">
            <div className="tbd">
              清理动作（只进回收站）属于 P7 文件操作矩阵；这里只给判定与证据，不做任何删除。
            </div>
          </div>
        </>
      ) : null}
    </>
  );
}

/* ── 变更集（虚拟变更集） ─────────────────────────────────────────── */

const CHECK_LABEL: Record<string, string> = {
  conflict: '冲突',
  illegal: '名字非法',
  tooLong: '路径超长',
  outOfRoot: '超出工作根',
  protected: '受保护',
  referenced: '被引用',
};

export function DraftsPanel({ onClose }: { onClose: () => void }) {
  const [list, setList] = useState<DraftList | null>(null);
  const [err, setErr] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  /** §7.6 / §16④：「已跳过 N 项（受保护）」明细，可点开逐条查看。 */
  const [skipped, setSkipped] = useState<SkippedProtected[] | null>(null);
  const [showSkipped, setShowSkipped] = useState(false);

  const act = async (fn: () => Promise<DraftList>) => {
    setBusy(true);
    setErr(null);
    try {
      setList(await fn());
    } catch (e) {
      setErr(errorText(e));
    } finally {
      setBusy(false);
    }
  };

  const loadSkipped = async () => {
    try {
      setSkipped(await api.skippedProtectedList());
    } catch (e) {
      setErr(errorText(e));
    }
  };

  useEffect(() => {
    void act(api.draftList).then(() => loadSkipped());
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  return (
    <>
      {skipped && skipped.length > 0 ? (
        <div className="detail-sec">
          <button className="btn" type="button" onClick={() => setShowSkipped((v) => !v)}>
            已跳过 {formatCount(skipped.length)} 项（受保护）
          </button>
          {showSkipped ? (
            <div style={{ marginTop: 'var(--spacing-2)' }}>
              {skipped.map((s) => (
                <div key={s.assetId} style={{ marginBottom: 'var(--spacing-2)' }}>
                  <div className="kv-key">
                    {s.name} · {s.addedBy === 'auto' ? '提交成功后自动加入' : '手工加入'} ·{' '}
                    {formatDateTime(s.addedAt)}
                  </div>
                  <div className="kv-val mono">{s.relPath}</div>
                  {s.reason ? <div className="tbd">{s.reason}</div> : null}
                </div>
              ))}
              <div className="tbd">
                受保护项在批量操作中被默认跳过（§7.6）；需要时可在右栏「保护区…」里逐条移出。
              </div>
            </div>
          ) : null}
        </div>
      ) : null}

      <div className="detail-sec">
        <div className="detail-title">变更集（虚拟变更集）</div>
        <div className="kv">
          <div className="kv-key">待提交 / 有问题</div>
          <div className="kv-val">
            {formatCount(list?.count ?? 0)} 项 / {formatCount(list?.problems ?? 0)} 项
          </div>
        </div>
        <div style={{ marginTop: 'var(--spacing-2)' }}>
          <button className="btn" type="button" disabled={busy || !list?.count} onClick={() => void act(api.draftUndo)}>
            撤销（Ctrl+Z）
          </button>{' '}
          <button className="btn" type="button" disabled={busy} onClick={() => void act(api.draftRedo)}>
            重做（Ctrl+Y）
          </button>{' '}
          <button className="btn" type="button" disabled={busy || !list?.count} onClick={() => void act(api.draftClear)}>
            放弃全部…
          </button>{' '}
          <Back onClose={onClose} />
        </div>
      </div>

      {err ? (
        <div className="detail-sec">
          <div className="tbd">{err}</div>
        </div>
      ) : null}

      {list && list.drafts.length > 0 ? (
        <div className="detail-sec">
          <div className="detail-title">草稿（新的在上面）</div>
          {list.drafts
            .slice()
            .reverse()
            .slice(0, 30)
            .map((d) => (
              <div key={d.seq} style={{ marginBottom: 'var(--spacing-2)' }}>
                <div className="kv-key">
                  #{d.seq} {d.op}
                  {d.check !== 'ok' ? ` · ${CHECK_LABEL[d.check] ?? d.check}` : ''}
                </div>
                <div className="kv-val mono">
                  {d.src}
                </div>
                {d.dst ? (
                  <div className="kv-val mono">
                    → {d.dst}
                  </div>
                ) : null}
                {d.reason ? <div className="tbd">{d.reason}</div> : null}
              </div>
            ))}
        </div>
      ) : (
        <div className="detail-sec">
          <div className="tbd">无草稿</div>
        </div>
      )}

      <div className="detail-sec">
        <div className="tbd">
          提交执行（生成计划 → 最终预览 → 二次确认 → 落盘）属 P6；本阶段只做草稿与预检。
        </div>
      </div>
    </>
  );
}

/* ── 重新定位素材树 ───────────────────────────────────────────────── */

export function RelocateWizard({
  root,
  onClose,
  onNotice,
}: {
  root: string | null;
  onClose: () => void;
  onNotice: (n: NoticePayload) => void;
}) {
  const [oldRoot, setOldRoot] = useState(root ?? '');
  const [newRoot, setNewRoot] = useState('');
  const [plan, setPlan] = useState<RelocatePlan | null>(null);
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState<string | null>(null);
  const [lib, setLib] = useState<LibraryInfo | null>(null);

  useEffect(() => {
    api
      .libraryInfo()
      .then(setLib)
      .catch(() => setLib(null));
  }, []);

  const buildPlan = async () => {
    setBusy(true);
    setErr(null);
    try {
      setPlan(await api.relocatePlan(oldRoot, newRoot));
    } catch (e) {
      setErr(errorText(e));
      setPlan(null);
    } finally {
      setBusy(false);
    }
  };

  const apply = async (includeConfirm: boolean) => {
    setBusy(true);
    setErr(null);
    try {
      const changed = await api.relocateApply(oldRoot, newRoot, includeConfirm);
      onNotice({
        kind: 'info',
        title: '素材树已重新定位',
        lines: [
          `本次改写索引 ${formatCount(changed)} 条${includeConfirm ? '（含待确认一档）' : '（仅高置信一档）'}。`,
          '分组 / 标签 / 保护区 / 已整理标记都挂在条目 id 上，未受影响。',
          '建议重新扫描一次，让界面上的列表与索引对齐。',
        ],
      });
      await buildPlan();
    } catch (e) {
      setErr(errorText(e));
    } finally {
      setBusy(false);
    }
  };

  return (
    <>
      <div className="detail-sec">
        <div className="detail-title">
          <FolderSearch size={14} strokeWidth={1.75} aria-hidden /> 重新定位素材树
        </div>
        <div className="kv">
          <div className="kv-key">原素材根</div>
          <div className="kv-val">
            <input
              type="text"
              value={oldRoot}
              onChange={(e) => setOldRoot(e.target.value)}
              style={{ width: '100%' }}
            />
          </div>
          <div className="kv-key">新素材根</div>
          <div className="kv-val">
            <input
              type="text"
              value={newRoot}
              onChange={(e) => setNewRoot(e.target.value)}
              style={{ width: '100%' }}
            />
            <div style={{ marginTop: 'var(--spacing-2)' }}>
              <button
                className="btn"
                type="button"
                onClick={async () => {
                  const picked = await pickFolder();
                  if (picked) setNewRoot(picked);
                }}
              >
                选择新文件夹…
              </button>{' '}
              <button className="btn" type="button" disabled={busy || !oldRoot || !newRoot} onClick={() => void buildPlan()}>
                {busy ? '处理中…' : '生成计划'}
              </button>{' '}
              <Back onClose={onClose} />
            </div>
          </div>
        </div>
      </div>

      {err ? (
        <div className="detail-sec">
          <div className="tbd">{err}</div>
        </div>
      ) : null}

      {plan ? (
        <>
          <div className="detail-sec">
            <div className="detail-title">匹配结果</div>
            <div className="kv">
              <div className="kv-key">高置信</div>
              <div className="kv-val">{formatCount(plan.high)} 条（相对结构与体积都对得上）</div>
              <div className="kv-key">待确认</div>
              <div className="kv-val">{formatCount(plan.needsConfirm)} 条（按文件名+体积+时间匹配）</div>
              <div className="kv-key">无法匹配</div>
              <div className="kv-val">{formatCount(plan.unmatched)} 条（保持缺失，元数据不删）</div>
              <div className="kv-key">卷标识</div>
              <div className="kv-val mono">
                {plan.oldVolume} → {plan.newVolume}
                {plan.oldVolume === plan.newVolume ? '（同卷）' : '（跨卷）'}
              </div>
            </div>
            <div style={{ marginTop: 'var(--spacing-2)' }}>
              <button
                className="btn"
                type="button"
                disabled={busy || plan.high === 0}
                onClick={() => void apply(false)}
              >
                应用高置信（{formatCount(plan.high)} 条）
              </button>{' '}
              <button
                className="btn"
                type="button"
                disabled={busy || plan.high + plan.needsConfirm === 0}
                onClick={() => void apply(true)}
              >
                连待确认一起应用
              </button>
            </div>
          </div>

          {plan.matches.length > 0 ? (
            <div className="detail-sec">
              <div className="detail-title">逐条依据（前 {Math.min(plan.matches.length, 20)} 条）</div>
              {plan.matches.slice(0, 20).map((m) => (
                <div key={m.assetId} style={{ marginBottom: 'var(--spacing-2)' }}>
                  <div className="kv-key">【{CONFIDENCE_LABEL[m.confidence] ?? m.confidence}】{m.why}</div>
                  <div className="kv-val mono">
                    {m.oldRelPath}
                  </div>
                  {m.newRelPath ? (
                    <div className="kv-val mono">
                      → {m.newRelPath}
                    </div>
                  ) : null}
                </div>
              ))}
            </div>
          ) : null}
        </>
      ) : null}

      {lib ? (
        <div className="detail-sec">
          <div className="detail-title">库位置</div>
          <div className="kv">
            <div className="kv-key">位置形态</div>
            <div className="kv-val">{lib.location === 'portable' ? '程序目录 data（便携）' : '应用数据目录（默认）'}</div>
            <div className="kv-key">库根</div>
            <div className="kv-val mono">{lib.root}</div>
            <div className="kv-key">索引库</div>
            <div className="kv-val mono">{lib.db}</div>
            <div className="kv-key">缩略图缓存</div>
            <div className="kv-val mono">{lib.thumbs}</div>
          </div>
          {lib.degraded ? <div className="tbd">{lib.degraded}</div> : null}
        </div>
      ) : null}
    </>
  );
}
