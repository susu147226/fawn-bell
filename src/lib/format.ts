/** 展示层格式化工具（纯函数，无副作用）。 */

const BYTE_UNITS = ['B', 'KB', 'MB', 'GB', 'TB'];

/** 人类可读体积；保留两位小数（与 CLI 输出保持一致）。 */
export function formatBytes(n: number): string {
  if (!Number.isFinite(n) || n < 0) return '—';
  let v = n;
  let i = 0;
  while (v >= 1024 && i < BYTE_UNITS.length - 1) {
    v /= 1024;
    i += 1;
  }
  if (i === 0) return `${n} B`;
  return `${v.toFixed(2)} ${BYTE_UNITS[i]}`;
}

const COUNT_FMT = new Intl.NumberFormat('zh-CN');

/** 千分位计数（§7.1 顶部统计「图片 1,820 · 视频 96」）。 */
export function formatCount(n: number): string {
  if (!Number.isFinite(n)) return '—';
  return COUNT_FMT.format(n);
}

/** 时长：<1 s 用毫秒，其余用秒；用于扫描耗时。 */
export function formatDuration(ms: number): string {
  if (!Number.isFinite(ms) || ms < 0) return '—';
  if (ms < 1000) return `${Math.round(ms)} ms`;
  if (ms < 60_000) return `${(ms / 1000).toFixed(1)} s`;
  const m = Math.floor(ms / 60_000);
  const s = Math.round((ms % 60_000) / 1000);
  return `${m} 分 ${s} 秒`;
}

/** 预估剩余时间；`null` 表示样本不足。 */
export function formatEta(ms: number | null): string {
  if (ms === null || !Number.isFinite(ms)) return '正在估算';
  if (ms <= 0) return '即将完成';
  return `约还需 ${formatDuration(ms)}`;
}

function pad2(n: number): string {
  return n < 10 ? `0${n}` : String(n);
}

/** 列用的紧凑时间：同一年只显示月-日 时:分。 */
export function formatTime(ms: number): string {
  if (!ms) return '—';
  const d = new Date(ms);
  if (Number.isNaN(d.getTime())) return '—';
  const now = new Date();
  const sameYear = d.getFullYear() === now.getFullYear();
  const base = `${pad2(d.getMonth() + 1)}-${pad2(d.getDate())} ${pad2(d.getHours())}:${pad2(d.getMinutes())}`;
  return sameYear ? base : `${d.getFullYear()}-${base}`;
}

/** 详情面板用的完整时间。 */
export function formatDateTime(ms: number): string {
  if (!ms) return '—';
  const d = new Date(ms);
  if (Number.isNaN(d.getTime())) return '—';
  return `${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())} ${pad2(
    d.getHours(),
  )}:${pad2(d.getMinutes())}:${pad2(d.getSeconds())}`;
}

const collator = new Intl.Collator('zh-Hans-CN', { numeric: true, sensitivity: 'base' });

/** 自然序比较（数字段按数值比较，中文按拼音）；资源管理器的排序手感。 */
export function compareName(a: string, b: string): number {
  return collator.compare(a, b);
}

/** 取路径末段（同时兼容 `\` 与 `/`）。 */
export function basename(p: string): string {
  const s = p.replace(/[\\/]+$/, '');
  const i = Math.max(s.lastIndexOf('\\'), s.lastIndexOf('/'));
  return i >= 0 ? s.slice(i + 1) : s;
}

/** 把绝对路径按分隔符拆成可展示的层级（只用于面包屑与详情）。 */
export function splitPath(p: string): string[] {
  return p.split(/[\\/]+/).filter(Boolean);
}

/**
 * 超长路径中段省略（§8.5：超长路径中段省略，悬停显示全路径）。
 * 保留头尾，中间用 `…` 替换。
 */
export function ellipsizeMiddle(s: string, max = 64): string {
  if (s.length <= max) return s;
  const head = Math.ceil((max - 1) * 0.55);
  const tail = max - 1 - head;
  return `${s.slice(0, head)}…${s.slice(s.length - tail)}`;
}
