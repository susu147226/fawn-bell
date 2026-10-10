/**
 * 保护区面板（执行版 §7.6）。
 *
 * 保护区是**横切安全属性**，不是分组：本面板只读库里的 `protections` 表，
 * 逐条显示「加入时间 + 加入方式（自动 / 手工）+ 原因」，支持单条移出；
 * 「全部移出」按条文要求走**二次确认**。
 *
 * 边界：受保护项在批量操作里被默认跳过（§7.6），跳过的具体落地在变更集预检与提交预览里，
 * 本面板只负责「看清有谁、随时移出」。
 */

import { useEffect, useState } from 'react';

import { formatCount, formatDateTime } from '../lib/format';
import { api, errorText } from '../lib/ipc';
import type { Protection, ProtectionStats } from '../lib/types';

const BY_LABEL: Record<string, string> = {
  auto: '提交成功后自动加入',
  manual: '手工加入',
};

export default function ProtectionPanel({
  onClose,
  onChanged,
}: {
  onClose: () => void;
  onChanged?: () => void;
}) {
  const [rows, setRows] = useState<Protection[] | null>(null);
  const [stats, setStats] = useState<ProtectionStats | null>(null);
  const [confirmAll, setConfirmAll] = useState(false);
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState<string | null>(null);

  const load = async () => {
    try {
      const [list, s] = await Promise.all([api.protectedList(), api.protectionStats()]);
      setRows(list);
      setStats(s);
      setErr(null);
    } catch (e) {
      setErr(errorText(e));
    }
  };

  useEffect(() => {
    void load();
  }, []);

  const removeOne = async (assetId: number) => {
    setBusy(true);
    try {
      await api.protectedRemove([assetId]);
      await load();
      onChanged?.();
    } catch (e) {
      setErr(errorText(e));
    } finally {
      setBusy(false);
    }
  };

  const removeAll = async () => {
    setBusy(true);
    try {
      await api.protectedRemoveAll();
      setConfirmAll(false);
      await load();
      onChanged?.();
    } catch (e) {
      setErr(errorText(e));
    } finally {
      setBusy(false);
    }
  };

  return (
    <>
      <div className="detail-sec">
        <div className="detail-title">保护区</div>
        <div className="kv">
          <div className="kv-key">条目数 / 当周新增</div>
          <div className="kv-val">
            共 {formatCount(stats?.total ?? 0)} 项 · 当周新增 {formatCount(stats?.weekNew ?? 0)}
          </div>
        </div>
        <div style={{ marginTop: 'var(--spacing-2)' }}>
          <button className="btn" type="button" disabled={busy || !rows?.length} onClick={() => setConfirmAll(true)}>
            全部移出…
          </button>{' '}
          <button className="btn" type="button" onClick={onClose}>
            返回
          </button>
        </div>
      </div>

      {confirmAll ? (
        <div className="detail-sec">
          <div className="notice warn">
            <div className="notice-body">
              <div className="notice-title">
                确定移出全部 {formatCount(stats?.total ?? 0)} 项？
              </div>
              <ul className="notice-list">
                <li>移出后这些素材将不再被批量操作默认跳过。</li>
                <li>真实文件不会发生任何变化（保护区只存在本地库里）。</li>
              </ul>
            </div>
          </div>
          <button className="btn danger" type="button" disabled={busy} onClick={() => void removeAll()}>
            确认全部移出
          </button>{' '}
          <button className="btn" type="button" onClick={() => setConfirmAll(false)}>
            再想想
          </button>
        </div>
      ) : null}

      {err ? (
        <div className="detail-sec">
          <div className="tbd">出错了：{err}</div>
        </div>
      ) : null}

      <div className="detail-sec">
        <div className="detail-title">受保护条目（新的在上面）</div>
        {rows && rows.length > 0 ? (
          rows.slice(0, 50).map((r) => (
            <div key={r.assetId} style={{ marginBottom: 'var(--spacing-2)' }}>
              <div className="kv-key">
                素材 #{r.assetId} · {BY_LABEL[r.addedBy] ?? r.addedBy} · {formatDateTime(r.addedAt)}
              </div>
              {r.reason ? <div className="tbd">{r.reason}</div> : null}
              <button className="btn" type="button" disabled={busy} onClick={() => void removeOne(r.assetId)}>
                移出
              </button>
            </div>
          ))
        ) : (
          <div className="tbd">还没有受保护条目</div>
        )}
      </div>
    </>
  );
}
