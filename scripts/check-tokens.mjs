#!/usr/bin/env node
/*
 * 鹿铃 · 令牌静态检查（执行版 §12.4 落地约束② / §9.1）
 * =====================================================================
 * §12.4② 原文要求：「构建期加一条静态检查（检索编译产物与源码中的十六进制色值
 * 与像素时长字面量），在本地打包脚本中执行，命中即报错。」
 *
 * 本脚本即该检查，规则如下（命中任一即 exit 1）：
 *   hex-color      源码与构建产物里的色值，必须能在 src/styles/tokens.css 找到同名登记
 *   color-func     同上，rgb() / rgba() / hsl() / hsla()
 *   time-literal   时长字面量（160ms、.16s…）必须来自 tokens.css（§12.4④ 上限 250ms）
 *   var-undefined  var(--x) 用到的令牌必须在 tokens.css 声明（或同文件内自声明）
 *   remote-url     §12.4① 零网络：源码与产物不得出现远程地址 / 远程字体 / CDN
 *   token-missing  §9.1 令牌清单必须齐备，且深色主题必须重新声明全部颜色令牌
 *
 * 判定口径（为什么不是简单字符串比对）：
 *   1. 色值与时长一律**先归一化再比对**——压缩器会把 `rgba(16,24,40,.06)` 改写成
 *      `#1018280f`、把 `160ms` 改写成 `.16s`，字符串不同但语义相同，必须判为合规。
 *   2. `transparent` 是关键字（压缩后成为 `#0000`），不是写死的色值，计入允许集。
 *   3. 比对前剥掉注释（`/* *​/` 与 ts/tsx 的行注释）——文档性注释里会举例写色值。
 *      但 remote-url 规则用**未剥注释的原文**，避免注释剥离顺手掩盖远程地址。
 *   4. docs/ 是设计稿件（mockups 里本就有字面色值），不属交付源码，整体跳过。
 *
 * 用法：
 *   node scripts/check-tokens.mjs            检查源码 + 构建产物（默认）
 *   node scripts/check-tokens.mjs --src      只检查源码
 *   node scripts/check-tokens.mjs --dist     只检查构建产物
 */

import { existsSync, readFileSync, readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { extname, join } from 'node:path';

const ROOT = fileURLToPath(new URL('../', import.meta.url));
const TOKENS_FILE = join(ROOT, 'src', 'styles', 'tokens.css');

const SKIP_DIRS = new Set([
  'node_modules',
  '.git',
  'target',
  'dist',
  '.dsh-project-memory',
  '.tmp-extract',
  'docs',
]);
const SOURCE_EXT = new Set(['.css', '.ts', '.tsx']);
const DIST_EXT = new Set(['.css', '.js', '.html']);

const COLOR_RE = /#[0-9a-fA-F]{3,8}\b|\b(?:rgba?|hsla?)\([^)]*\)/gi;
const TIME_RE = /(?<![\w.-])(\d*\.?\d+)(ms|s)\b/gi;
const REMOTE_RE = /https?:\/\//i;
const DIST_REMOTE_LOAD_RE = /(?:url\(\s*['"]?https?:\/\/|@import\s+(?:url\()?\s*['"]?https?:\/\/)/i;
const DIST_REMOTE_HOST_RE =
  /(?:cdn\.|unpkg\.com|jsdelivr\.net|fonts\.googleapis\.com|fonts\.gstatic\.com|cdnjs\.cloudflare\.com)/i;
const VAR_USE_RE = /var\(\s*(--[a-z0-9-]+)/gi;
const VAR_DECL_RE = /(--[a-z0-9-]+)\s*:/gi;

/** §9.1 令牌清单（执行版 662 行起的唯一权威定义）。 */
const REQUIRED_COLORS = [
  'bg',
  'surface',
  'surface-alt',
  'border',
  'text-primary',
  'text-secondary',
  'text-muted',
  'accent',
  'accent-hover',
  'success',
  'warning',
  'danger',
  'info',
  'draft-mark',
  'protected-mark',
  'organized-mark',
];
const REQUIRED_SIZES = [
  'radius-sm',
  'radius-md',
  'radius-lg',
  'spacing-1',
  'spacing-2',
  'spacing-3',
  'spacing-4',
  'spacing-5',
  'spacing-6',
  'row-height',
  'thumb-size',
  'icon-size',
  'font-size-1',
  'font-size-2',
  'font-size-3',
  'font-size-4',
  'font-size-5',
  'font-size-6',
];
const REQUIRED_OTHER = [
  'shadow-1',
  'shadow-2',
  'shadow-3',
  'motion-duration',
  'font-family',
  'font-mono',
  'density',
];

const args = process.argv.slice(2);
const wanted = { src: !args.includes('--dist'), dist: !args.includes('--src') };

const problems = [];

// ── 归一化 ─────────────────────────────────────────────────────────────
function hslToRgb(h, s, l) {
  const hh = (((h % 360) + 360) % 360) / 360;
  const ss = s / 100;
  const ll = l / 100;
  if (ss === 0) {
    const v = Math.round(ll * 255);
    return [v, v, v];
  }
  const q = ll < 0.5 ? ll * (1 + ss) : ll + ss - ll * ss;
  const p = 2 * ll - q;
  const f = (t) => {
    let x = t;
    if (x < 0) x += 1;
    if (x > 1) x -= 1;
    if (x < 1 / 6) return p + (q - p) * 6 * x;
    if (x < 1 / 2) return q;
    if (x < 2 / 3) return p + (q - p) * (2 / 3 - x) * 6;
    return p;
  };
  return [Math.round(f(hh + 1 / 3) * 255), Math.round(f(hh) * 255), Math.round(f(hh - 1 / 3) * 255)];
}

function alphaByte(a) {
  return Math.round(Math.max(0, Math.min(1, a)) * 255);
}

/** 把任意色值字面量归一成 `rgba(r,g,b,A)`（A 为 0–255 整数），便于跨写法比对。 */
function canonColor(literal) {
  const s = literal.trim().toLowerCase();
  if (s === 'transparent') return 'rgba(0,0,0,0)';
  if (s.startsWith('#')) {
    let h = s.slice(1);
    if (h.length === 3 || h.length === 4) h = [...h].map((c) => c + c).join('');
    if (h.length === 6) h += 'ff';
    if (h.length !== 8) return s;
    return `rgba(${parseInt(h.slice(0, 2), 16)},${parseInt(h.slice(2, 4), 16)},${parseInt(h.slice(4, 6), 16)},${parseInt(h.slice(6, 8), 16)})`;
  }
  const rgb = /^rgba?\(([^)]*)\)$/.exec(s);
  if (rgb) {
    const parts = rgb[1].split(/[\s,/]+/).filter(Boolean);
    const chan = (p) => (p.endsWith('%') ? Math.round((parseFloat(p) / 100) * 255) : Math.round(parseFloat(p)));
    const [r, g, b] = parts.slice(0, 3).map(chan);
    const a = parts.length > 3 ? parseFloat(parts[3]) : 1;
    return `rgba(${r},${g},${b},${alphaByte(a)})`;
  }
  const hsl = /^hsla?\(([^)]*)\)$/.exec(s);
  if (hsl) {
    const parts = hsl[1].split(/[\s,/]+/).filter(Boolean);
    const [r, g, b] = hslToRgb(parseFloat(parts[0]), parseFloat(parts[1]), parseFloat(parts[2]));
    const a = parts.length > 3 ? parseFloat(parts[3]) : 1;
    return `rgba(${r},${g},${b},${alphaByte(a)})`;
  }
  return s;
}

/** 时长归一成毫秒数（`160ms` 与 `.16s` 都是 160）。 */
function canonTime(value, unit) {
  const n = parseFloat(value) * (unit.toLowerCase() === 'ms' ? 1 : 1000);
  return String(Math.round(n * 1000) / 1000);
}

function stripComments(text, ext) {
  let out = text.replace(/\/\*[\s\S]*?\*\//g, ' ');
  if (ext === '.ts' || ext === '.tsx') out = out.replace(/(^|[^:\w])\/\/[^\n]*/g, '$1 ');
  return out;
}

// ── 令牌真源：从 tokens.css 提取「允许出现的字面量」 ──────────────────────
if (!existsSync(TOKENS_FILE)) {
  console.error(`[check-tokens] 找不到令牌文件：${TOKENS_FILE}`);
  process.exit(1);
}
const tokensText = stripComments(readFileSync(TOKENS_FILE, 'utf8'), '.css');
const allowedColors = new Set((tokensText.match(COLOR_RE) ?? []).map(canonColor));
allowedColors.add(canonColor('transparent'));
const allowedTimes = new Set(
  [...tokensText.matchAll(TIME_RE)].map((m) => canonTime(m[1], m[2])),
);
const declaredVars = new Set(
  (tokensText.match(VAR_DECL_RE) ?? []).map((s) => s.toLowerCase().replace(/\s*:$/, '')),
);

// ── 文件发现 / 报告 ────────────────────────────────────────────────────
function rel(p) {
  return p.slice(ROOT.length).replace(/\\/g, '/');
}

function lineOf(text, index) {
  let n = 1;
  for (let i = 0; i < index; i += 1) if (text.charCodeAt(i) === 10) n += 1;
  return n;
}

function report(rule, file, line, detail) {
  problems.push({ rule, file: rel(file), line, detail });
}

function walk(dir, exts, out = []) {
  if (!existsSync(dir)) return out;
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    const full = join(dir, entry.name);
    if (entry.isDirectory()) {
      if (SKIP_DIRS.has(entry.name)) continue;
      walk(full, exts, out);
    } else if (entry.isFile() && exts.has(extname(entry.name).toLowerCase())) {
      out.push(full);
    }
  }
  return out;
}

// ── §9.1 清单齐备性 ────────────────────────────────────────────────────
for (const name of [...REQUIRED_COLORS, ...REQUIRED_SIZES, ...REQUIRED_OTHER]) {
  if (!declaredVars.has(`--${name}`)) {
    report('token-missing', TOKENS_FILE, 1, `§9.1 令牌 --${name} 未在 tokens.css 声明`);
  }
}
const darkBlock = tokensText.match(/:root\[data-theme=['"]dark['"]\]\s*\{([\s\S]*?)\}/);
if (!darkBlock) {
  report('token-missing', TOKENS_FILE, 1, "缺少深色主题块 :root[data-theme='dark']");
} else {
  const darkVars = new Set(
    (darkBlock[1].match(VAR_DECL_RE) ?? []).map((s) => s.toLowerCase().replace(/\s*:$/, '')),
  );
  for (const name of REQUIRED_COLORS) {
    if (!darkVars.has(`--${name}`)) {
      report('token-missing', TOKENS_FILE, 1, `深色主题未重新声明颜色令牌 --${name}（§8.2 深色重新取色）`);
    }
  }
}

// ── 逐文件检查 ─────────────────────────────────────────────────────────

/**
 * 已知例外：Tailwind v4 会在产物里输出一段**能力探测**文本
 *   `@supports (not (color: rgb(from red r g b))) { … }`
 * 它用来判断浏览器是否支持相对颜色语法（决定要不要发 @property 兼容层），既不是组件写死的
 * 色值，也不可能影响主题。这里按**精确区间**豁免，不做任何模糊放行：只有落在该探测文本内的
 * 颜色匹配才跳过，其它位置照旧一律报错。
 */
const FEATURE_PROBES = [/@supports[^{]*rgb\(from\s+red\s+r\s+g\s+b\)[^{]*/g];

function inFeatureProbe(text, index) {
  for (const re of FEATURE_PROBES) {
    for (const m of text.matchAll(re)) {
      if (index >= m.index && index < m.index + m[0].length) return true;
    }
  }
  return false;
}

function checkFile(file) {
  const raw = readFileSync(file, 'utf8');
  const ext = extname(file).toLowerCase();
  const text = stripComments(raw, ext);
  const isTokens = file === TOKENS_FILE;

  if (!isTokens) {
    for (const m of text.matchAll(COLOR_RE)) {
      if (!allowedColors.has(canonColor(m[0])) && !inFeatureProbe(text, m.index)) {
        const rule = m[0].startsWith('#') ? 'hex-color' : 'color-func';
        report(rule, file, lineOf(text, m.index), `${m[0]} 不在 tokens.css 登记（§12.4② 组件内禁止写死色值）`);
      }
    }
    for (const m of text.matchAll(TIME_RE)) {
      if (!allowedTimes.has(canonTime(m[1], m[2]))) {
        report('time-literal', file, lineOf(text, m.index), `${m[0]} 不在 tokens.css 登记（§12.4② 禁止写死时长）`);
      }
    }
  }

  if (ext === '.css' || ext === '.tsx') {
    const localDecls = new Set(
      (text.match(VAR_DECL_RE) ?? []).map((s) => s.toLowerCase().replace(/\s*:$/, '')),
    );
    for (const m of text.matchAll(VAR_USE_RE)) {
      const name = m[1].toLowerCase();
      if (!declaredVars.has(name) && !localDecls.has(name)) {
        report('var-undefined', file, lineOf(text, m.index), `var(${m[1]}) 未在 tokens.css 声明（§9.1 令牌是唯一真源）`);
      }
    }
  }

  const inSrc = file.startsWith(join(ROOT, 'src')) || file === join(ROOT, 'index.html');
  if (inSrc) {
    const m = REMOTE_RE.exec(raw);
    if (m) report('remote-url', file, lineOf(raw, m.index), `${m[0]} —— §12.4① 禁止任何远程地址`);
  } else {
    let m = DIST_REMOTE_LOAD_RE.exec(raw);
    if (m) report('remote-url', file, lineOf(raw, m.index), `${m[0].trim()} —— §12.4① 禁止远程加载样式 / 字体`);
    m = DIST_REMOTE_HOST_RE.exec(raw);
    if (m) report('remote-url', file, lineOf(raw, m.index), `${m[0]} —— §12.4① 构建产物不得含 CDN / 网络字体`);
  }
}

const srcFiles = wanted.src
  ? [
      ...walk(join(ROOT, 'src'), SOURCE_EXT),
      ...(existsSync(join(ROOT, 'index.html')) ? [join(ROOT, 'index.html')] : []),
    ]
  : [];
const distFiles = wanted.dist ? walk(join(ROOT, 'dist'), DIST_EXT) : [];
// 产物先查（默认可编辑源码起，产物只在发布构建时才有意义）
for (const file of [...distFiles, ...srcFiles]) checkFile(file);

// ── 结果 ───────────────────────────────────────────────────────────────
console.log(
  `[check-tokens] 源码 ${srcFiles.length} 个 / 构建产物 ${distFiles.length} 个文件｜真源 ${rel(TOKENS_FILE)}`,
);
if (problems.length === 0) {
  console.log('[check-tokens] OK · 0 命中（§9.1 令牌齐备、零网络、无未登记字面量）');
  process.exit(0);
}
const order = ['hex-color', 'color-func', 'time-literal', 'var-undefined', 'remote-url', 'token-missing'];
problems.sort(
  (a, b) =>
    order.indexOf(a.rule) - order.indexOf(b.rule) || a.file.localeCompare(b.file) || a.line - b.line,
);
console.error(`[check-tokens] 命中 ${problems.length} 处 —— §12.4② 命中即报错：`);
for (const p of problems) console.error(`  ${p.rule.padEnd(14)} ${p.file}:${p.line}  ${p.detail}`);
process.exit(1);
