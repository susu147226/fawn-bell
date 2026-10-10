/**
 * 命名面板（执行版 §7.3.2 / §7.3.3）。
 *
 * 三块内容：
 * ① **预设**：内置 13 个标签，点一下即套用 `<基名>_{seq}`（固定契约，§7.3.2）；自定义预设可保存/删除，
 *    可导出成 JSON 自己留存、也可粘回来导入。全部只写库目录，**不落素材目录**（§14①）。
 * ② **序号设置**：起始值 0/1、补零档位（不补零/最少 2 位/最少 3 位/固定 N 位）、序号前分隔符、
 *    作用域、原名序号剥离——默认值与核心域 `SeqRule::default()` 一致。
 * ③ **实时预览**：模板或任一设置变动都**立刻**重算前三项（§7.3.3 第 2 点：避免把 `0` 误读成空序号）。
 *
 * 边界：本面板只改「命名规则」，**不排草稿、不碰磁盘**；真正排计划走 §7.2 的草稿 → 预检 → 提交管线。
 */

import { useEffect, useState } from 'react';

import { api, errorText } from '../lib/ipc';
import {
  DEFAULT_SEQ_RULE,
  type PadMode,
  type Preset,
  type Rendered,
  type SeqRule,
  type SeqSep,
  type StripRule,
} from '../lib/types';

function padLabel(p: PadMode): string {
  if (p === 'noPad') return '不补零（默认）';
  if (p === 'min2') return '最少 2 位';
  if (p === 'min3') return '最少 3 位';
  return `固定 ${p.fixed} 位`;
}

const SEP_LABEL: Record<SeqSep, string> = {
  autoUnderscore: '自动补 _（默认）',
  dash: '- 连字符',
  space: '空格',
  none: '不加分隔符',
};

const STRIP_LABEL: Record<StripRule, string> = {
  none: '不剥离',
  trailingDigits: '剥离尾随数字',
  trailingUnderscoreDigits: '剥离尾随 _数字（默认）',
  regex: '按正则剥离',
};

export default function NamingPanel({ onClose }: { onClose: () => void }) {
  const [presets, setPresets] = useState<Preset[] | null>(null);
  const [template, setTemplate] = useState('date_{seq}');
  const [rule, setRule] = useState<SeqRule>(DEFAULT_SEQ_RULE);
  const [stem, setStem] = useState('IMG_0001');
  const [ext, setExt] = useState('jpg');
  const [preview, setPreview] = useState<Rendered[] | null>(null);
  const [err, setErr] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const [newBase, setNewBase] = useState('');
  const [io, setIo] = useState('');
  const [ioNote, setIoNote] = useState<string | null>(null);

  const loadPresets = async () => {
    try {
      setPresets(await api.presetList());
    } catch (e) {
      setErr(errorText(e));
    }
  };

  useEffect(() => {
    void loadPresets();
  }, []);

  // 实时预览：模板 / 序号设置 / 样例任一变动立刻重算
  useEffect(() => {
    let alive = true;
    api
      .namingPreview(template, rule, stem, ext)
      .then((r) => {
        if (alive) setPreview(r);
      })
      .catch((e) => {
        if (alive) setErr(errorText(e));
      });
    return () => {
      alive = false;
    };
  }, [template, rule, stem, ext]);

  const applyPreset = async (p: Preset) => {
    setBusy(true);
    setErr(null);
    try {
      const r = await api.presetApply(p.id, ext, rule.start, rule);
      setTemplate(r.template);
    } catch (e) {
      setErr(errorText(e));
    } finally {
      setBusy(false);
    }
  };

  const saveCustom = async () => {
    const base = newBase.trim();
    if (!base) return;
    setBusy(true);
    setErr(null);
    try {
      await api.presetSave(null, base, null, template.trim() || `${base}_{seq}`);
      setNewBase('');
      await loadPresets();
      setIoNote(`已保存自定义预设「${base}」`);
    } catch (e) {
      setErr(errorText(e));
    } finally {
      setBusy(false);
    }
  };

  const removePreset = async (p: Preset) => {
    setBusy(true);
    setErr(null);
    try {
      await api.presetDelete(p.id);
      await loadPresets();
      setIoNote(`已删除预设「${p.baseName}」`);
    } catch (e) {
      setErr(errorText(e));
    } finally {
      setBusy(false);
      setIoNote(null);
    }
  };

  const doExport = async () => {
    try {
      setIo(await api.presetExport());
      setIoNote('已生成 JSON：复制走即可（软件不会自己往素材目录里写文件）');
    } catch (e) {
      setErr(errorText(e));
    }
  };

  const doImport = async () => {
    setBusy(true);
    setErr(null);
    try {
      const n = await api.presetImport(io);
      await loadPresets();
      setIoNote(`导入完成：新增/更新 ${n} 条自定义预设（内置 13 个保持不变）`);
    } catch (e) {
      setErr(errorText(e));
    } finally {
      setBusy(false);
    }
  };

  return (
    <>
      <div className="detail-sec">
        <div className="detail-title">命名规则</div>
        <div className="kv">
          <div className="kv-key">模板</div>
          <div className="kv-val">
            <input
              type="text"
              value={template}
              onChange={(e) => setTemplate(e.target.value)}
              placeholder="date_{seq}"
              style={{ width: '100%' }}
            />
          </div>
        </div>
      </div>

      <div className="detail-sec">
        <div className="detail-title">内置预设（点一下即套用 {`<基名>_{seq}`}）</div>
        <div style={{ display: 'flex', flexWrap: 'wrap', gap: 'var(--spacing-1)' }}>
          {(presets ?? []).filter((p) => p.isBuiltin).map((p) => (
            <button
              className="btn"
              type="button"
              key={p.id}
              disabled={busy}
              title={`套用为 ${p.template}`}
              onClick={() => void applyPreset(p)}
            >
              {p.baseName}
            </button>
          ))}
        </div>
        {presets && presets.some((p) => !p.isBuiltin) ? (
          <>
            <div className="detail-title" style={{ marginTop: 'var(--spacing-3)' }}>
              自定义预设
            </div>
            <div style={{ display: 'flex', flexWrap: 'wrap', gap: 'var(--spacing-1)' }}>
              {presets.filter((p) => !p.isBuiltin).map((p) => (
                <span key={p.id}>
                  <button className="btn" type="button" disabled={busy} onClick={() => setTemplate(p.template)}>
                    {p.label ?? p.baseName}
                  </button>{' '}
                  <button className="btn" type="button" disabled={busy} onClick={() => void removePreset(p)}>
                    删除
                  </button>
                </span>
              ))}
            </div>
          </>
        ) : null}
        <div className="kv" style={{ marginTop: 'var(--spacing-3)' }}>
          <div className="kv-key">存为自定义</div>
          <div className="kv-val">
            <input
              type="text"
              value={newBase}
              onChange={(e) => setNewBase(e.target.value)}
              placeholder="基名，如 wd_dot"
              style={{ width: '100%' }}
            />
            <div style={{ marginTop: 'var(--spacing-1)' }}>
              <button className="btn" type="button" disabled={busy || !newBase.trim()} onClick={() => void saveCustom()}>
                用当前模板存为预设
              </button>
            </div>
          </div>
        </div>
      </div>

      <div className="detail-sec">
        <div className="detail-title">序号规则</div>
        <div className="kv">
          <div className="kv-key">起始值</div>
          <div className="kv-val">
            <label>
              <input
                type="radio"
                name="seq-start"
                checked={rule.start === 0}
                onChange={() => setRule({ ...rule, start: 0 })}
              />{' '}
              从 0 开始（默认）
            </label>{' '}
            <label>
              <input
                type="radio"
                name="seq-start"
                checked={rule.start === 1}
                onChange={() => setRule({ ...rule, start: 1 })}
              />{' '}
              从 1 开始
            </label>
          </div>

          <div className="kv-key">补零位数</div>
          <div className="kv-val">
            <select
              value={typeof rule.pad === 'string' ? rule.pad : 'fixed'}
              onChange={(e) => {
                const v = e.target.value;
                const pad: PadMode =
                  v === 'noPad' || v === 'min2' || v === 'min3'
                    ? v
                    : { fixed: typeof rule.pad === 'object' ? rule.pad.fixed : 2 };
                setRule({ ...rule, pad });
              }}
            >
              <option value="noPad">不补零（默认）</option>
              <option value="min2">最少 2 位</option>
              <option value="min3">最少 3 位</option>
              <option value="fixed">固定 N 位</option>
            </select>{' '}
            {typeof rule.pad === 'object' ? (
              <input
                type="number"
                min={1}
                max={6}
                value={rule.pad.fixed}
                onChange={(e) => {
                  const n = Math.min(6, Math.max(1, Number(e.target.value) || 1));
                  setRule({ ...rule, pad: { fixed: n } });
                }}
              />
            ) : null}
          </div>

          <div className="kv-key">序号前分隔符</div>
          <div className="kv-val">
            <select value={rule.sep} onChange={(e) => setRule({ ...rule, sep: e.target.value as SeqSep })}>
              {Object.entries(SEP_LABEL).map(([k, v]) => (
                <option key={k} value={k}>
                  {v}
                </option>
              ))}
            </select>
          </div>

          <div className="kv-key">原名序号剥离</div>
          <div className="kv-val">
            <select value={rule.strip} onChange={(e) => setRule({ ...rule, strip: e.target.value as StripRule })}>
              {Object.entries(STRIP_LABEL).map(([k, v]) => (
                <option key={k} value={k}>
                  {v}
                </option>
              ))}
            </select>
          </div>

          <div className="kv-key">预览样例</div>
          <div className="kv-val">
            <input type="text" value={stem} onChange={(e) => setStem(e.target.value)} style={{ width: '60%' }} />{' '}
            <input type="text" value={ext} onChange={(e) => setExt(e.target.value)} style={{ width: '20%' }} />
          </div>
        </div>
        <div className="tbd" style={{ marginTop: 'var(--spacing-1)' }}>
          {`起始 ${rule.start} · ${padLabel(rule.pad)}`}
        </div>
        <div style={{ marginTop: 'var(--spacing-2)' }}>
          <button className="btn" type="button" onClick={onClose}>
            返回
          </button>
        </div>
      </div>

      {err ? (
        <div className="detail-sec">
          <div className="nothing">出错了：{err}</div>
        </div>
      ) : null}

      <div className="detail-sec">
        <div className="detail-title">实时预览（前三项）</div>
        {preview ? (
          preview.map((r, i) => (
            <div key={i} style={{ marginBottom: 'var(--spacing-1)' }}>
              <span className="mono">{r.name}</span>
              {r.notes.length > 0 ? <span className="tbd">　// {r.notes.join('；')}</span> : null}
            </div>
          ))
        ) : (
          <div className="tbd">正在计算…</div>
        )}
      </div>

      <div className="detail-sec">
        <div className="detail-title">预设导入 / 导出</div>
        <div>
          <button className="btn" type="button" onClick={() => void doExport()}>
            导出为 JSON
          </button>{' '}
          <button className="btn" type="button" disabled={busy || !io.trim()} onClick={() => void doImport()}>
            从下面的 JSON 导入
          </button>
        </div>
        <textarea
          value={io}
          onChange={(e) => setIo(e.target.value)}
          rows={6}
          placeholder="导出的预设 JSON 会出现在这里；也可以把别人的 JSON 粘进来再点导入"
          style={{ width: '100%', marginTop: 'var(--spacing-2)' }}
        />
        {ioNote ? <div className="tbd">{ioNote}</div> : null}
      </div>

      <div className="detail-sec">
        <button className="btn" type="button" onClick={onClose}>
          返回
        </button>
      </div>
    </>
  );
}
