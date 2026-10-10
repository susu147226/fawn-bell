/**
 * 归档面板（执行版 §7.4 归档与位置整理）。
 *
 * 只做「出计划」：目录模板 + 命名模板 + 冲突策略 + 空目录清理开关 → 预览表
 * （旧路径 → 新路径、同盘/跨盘、冲突结论、工程包联动项、被引用警告），
 * 并支持**逐行勾选取消**（§7.4：可在预览中逐项取消）。
 *
 * 边界：真正搬运（同盘元数据操作、跨盘「复制 → 校验 → 删除源 → 写 journal」）属 P6 提交执行，
 * 这里绝不碰素材树——按钮只到「生成计划」为止，并如实标注。
 */

import { useState } from 'react';

import { api, errorText } from '../lib/ipc';
import type { ArchivePlan, ConflictPolicy } from '../lib/types';

const VERDICT_LABEL: Record<string, string> = {
  free: '可直接落位',
  duplicateSkip: '重复 → 跳过',
  conflict: '冲突',
};

export default function ArchivePanel({
  root,
  folder,
  onClose,
}: {
  root: string | null;
  folder: string;
  onClose: () => void;
}) {
  const [target, setTarget] = useState('');
  const [dirTemplate, setDirTemplate] = useState('{kind}/{yyyy}/{mm}');
  const [nameTemplate, setNameTemplate] = useState('{name}_{seq}');
  const [policy, setPolicy] = useState<ConflictPolicy>('suffix');
  const [cleanEmpty, setCleanEmpty] = useState(false);
  const [plan, setPlan] = useState<ArchivePlan | null>(null);
  const [dropped, setDropped] = useState<Set<number>>(new Set());
  const [err, setErr] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  const build = async () => {
    if (!root || !target.trim()) return;
    setBusy(true);
    setErr(null);
    try {
      const p = await api.archivePlan(root, folder, target.trim(), dirTemplate, nameTemplate, policy, cleanEmpty);
      setPlan(p);
      setDropped(new Set());
    } catch (e) {
      setErr(errorText(e));
    } finally {
      setBusy(false);
    }
  };

  const toggle = (assetId: number) => {
    setDropped((prev) => {
      const next = new Set(prev);
      if (next.has(assetId)) next.delete(assetId);
      else next.add(assetId);
      return next;
    });
  };

  const kept = plan ? plan.items.filter((i) => !i.excluded && !dropped.has(i.assetId)).length : 0;

  return (
    <>
      <div className="detail-sec">
        <div className="detail-title">归档与位置整理</div>
        <div className="kv">
          <div className="kv-key">目标根</div>
          <div className="kv-val">
            <input
              type="text"
              value={target}
              placeholder="D:\\归档"
              onChange={(e) => setTarget(e.target.value)}
              style={{ width: '100%' }}
            />
          </div>
          <div className="kv-key">目录模板</div>
          <div className="kv-val">
            <input
              type="text"
              value={dirTemplate}
              onChange={(e) => setDirTemplate(e.target.value)}
              style={{ width: '100%' }}
            />
          </div>
          <div className="kv-key">命名模板</div>
          <div className="kv-val">
            <input
              type="text"
              value={nameTemplate}
              onChange={(e) => setNameTemplate(e.target.value)}
              style={{ width: '100%' }}
            />
          </div>
          <div className="kv-key">目标同名时</div>
          <div className="kv-val">
            <select value={policy} onChange={(e) => setPolicy(e.target.value as ConflictPolicy)}>
              <option value="suffix">加后缀（默认）</option>
              <option value="skip">跳过并记录</option>
              <option value="abortBatch">中止整个批次</option>
            </select>
          </div>
          <div className="kv-key">空目录清理</div>
          <div className="kv-val">
            <label>
              <input type="checkbox" checked={cleanEmpty} onChange={(e) => setCleanEmpty(e.target.checked)} /> 归档后清理
              变空的源目录
            </label>
          </div>
        </div>
        <div style={{ marginTop: 'var(--spacing-2)' }}>
          <button className="btn primary" type="button" disabled={busy || !root || !target.trim()} onClick={() => void build()}>
            生成计划（只读）
          </button>{' '}
          <button className="btn" type="button" onClick={onClose}>
            返回
          </button>
        </div>
      </div>

      {err ? (
        <div className="detail-sec">
          <div className="tbd">出错了：{err}</div>
        </div>
      ) : null}

      {plan ? (
        <>
          <div className="detail-sec">
            <div className="detail-title">计划结论</div>
            <div className="kv">
              <div className="kv-key">参与 / 保留</div>
              <div className="kv-val">
                {plan.items.filter((i) => !i.excluded).length} 项 → 保留 {kept} 项（逐行取消 {dropped.size} 项）
              </div>
              <div className="kv-key">空目录</div>
              <div className="kv-val">
                {plan.emptyDirs.length} 个 · 清理{plan.cleanEmptyDirs ? '开启' : '关闭（默认）'}
              </div>
            </div>
            {plan.notes.map((n) => (
              <div className="tbd" key={n}>
                {n}
              </div>
            ))}
            {plan.emptyDirs.length > 0 ? (
              <div className="tbd" style={{ marginTop: 'var(--spacing-1)' }}>
                将被清空的源目录：{plan.emptyDirs.slice(0, 6).join('、')}
              </div>
            ) : null}
          </div>

          <div className="detail-sec">
            <div className="detail-title">预览（可逐行取消）</div>
            {plan.items.slice(0, 80).map((it) => {
              const off = it.excluded || dropped.has(it.assetId);
              return (
                <div key={it.assetId} style={{ marginBottom: 'var(--spacing-2)' }}>
                  <label className="kv-key" style={{ display: 'block' }}>
                    <input
                      type="checkbox"
                      checked={!off}
                      disabled={it.excluded}
                      onChange={() => toggle(it.assetId)}
                    />{' '}
                    {it.src.split(/[\\/]/).pop()} → {it.dst || '（已剔除）'}
                  </label>
                  {!it.excluded ? (
                    <div className="tbd">
                      [{it.sameVolume ? '同盘' : '跨盘'}] · {VERDICT_LABEL[it.verdict] ?? it.verdict}
                      {it.companions.length > 0 ? ` · 工程包联动 ${it.companions.length} 项` : ''}
                    </div>
                  ) : null}
                  {it.notes.map((n) => (
                    <div className="tbd" key={n}>
                      {n}
                    </div>
                  ))}
                </div>
              );
            })}
          </div>
        </>
      ) : null}

      <div className="detail-sec">
        <button className="btn" type="button" onClick={onClose}>
          返回
        </button>
      </div>
    </>
  );
}
