#!/usr/bin/env node
/*
 * 鹿铃 · 依赖许可证逐版本核对（执行版 §12.4 落地约束⑤ / §5）
 * =====================================================================
 * §12.4⑤ 原文要求：「许可证逐版本核对，禁止凭记忆判定：打开该版本随包自带的
 * LICENSE / LICENSE.md / COPYING 文件，登记许可证名称与版本号；确认该许可证允许
 * 本项目『私有、禁商业、禁再分发』的分发方式；记入 README『许可与致谢』清单。」
 *
 * 本脚本把「打开随包 LICENSE 文件」这件事变成机器动作：
 *   1. 从 package.json 出发解析依赖树（dependencies 走传递闭包 = 会进产物的运行时依赖；
 *      devDependencies 只记直接项 = 构建期工具，不进发布产物）；
 *   2. 每个包读取其 node_modules 目录里真实存在的 LICENSE / LICENSE.md / COPYING；
 *   3. 登记版本号、LICENSE 文件名、许可证名称；
 *   4. 与包元数据（package.json 的 license 字段）交叉核对，不一致即报 REVIEW；
 *   5. 判定是否允许「私有、禁商业、禁再分发」的分发方式；
 *   6. 输出可直接粘进 README 的核对表（--readme 则就地写进 README 标记块）。
 *
 * 退出码：0 = 全部核对完成且均允许；1 = 存在缺 LICENSE / 许可证不允许 / 需人工判定。
 * 任何一项未核对完成都不得进入发布构建（§12.4⑤）。
 *
 * 用法：
 *   node scripts/license-audit.mjs                 打印核对表
 *   node scripts/license-audit.mjs --readme        写入 README.md 的标记块
 *   node scripts/license-audit.mjs --by 云舒眠眠    指定核对人（默认脚本名）
 */

import { existsSync, readFileSync, readdirSync, realpathSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { join } from 'node:path';

const ROOT = fileURLToPath(new URL('../', import.meta.url));
const NODE_MODULES = join(ROOT, 'node_modules');

/** 允许「私有、禁商业、禁再分发」分发方式的许可证。 */
const ALLOWED = new Set([
  'MIT',
  'MIT-0',
  'ISC',
  'Apache-2.0',
  'BSD-2-Clause',
  'BSD-3-Clause',
  '0BSD',
  'Unlicense',
  'CC0-1.0',
  'CC-BY-4.0',
  'OFL-1.1',
  'Zlib',
  'BlueOak-1.0.0',
]);

/** 明确不允许随本项目分发的许可证（copyleft / 非商业 / 专有）。 */
const FORBIDDEN = /^(A?GPL|LGPL|MPL|SSPL|BUSL|CC-BY-NC|CC-BY-SA|Elastic|Commons-Clause|JSON)/i;

/** LICENSE 文件正文 → 许可证名（只在元数据缺失或不一致时用来交叉核对）。 */
const TEXT_SIGNATURES = [
  [/MIT License|Permission is hereby granted, free of charge/i, 'MIT'],
  [/ISC License|Permission to use, copy, modify, and(\/or)? distribute this software/i, 'ISC'],
  [/Apache License\s*,?\s*Version 2\.0/i, 'Apache-2.0'],
  [/BSD 3-Clause|Neither the name of/i, 'BSD-3-Clause'],
  [/BSD 2-Clause|Redistribution and use in source and binary forms/i, 'BSD-2-Clause'],
  [/Mozilla Public License/i, 'MPL-2.0'],
  [/GNU AFFERO GENERAL PUBLIC LICENSE/i, 'AGPL-3.0'],
  [/GNU LESSER GENERAL PUBLIC LICENSE/i, 'LGPL-3.0'],
  [/GNU GENERAL PUBLIC LICENSE/i, 'GPL-3.0'],
  [/SIL OPEN FONT LICENSE/i, 'OFL-1.1'],
  [/Creative Commons Attribution 4\.0/i, 'CC-BY-4.0'],
  [/CC0 1\.0 Universal/i, 'CC0-1.0'],
  [/This is free and unencumbered software released into the public domain/i, 'Unlicense'],
  [/zlib License/i, 'Zlib'],
  [/Blue Oak Model License/i, 'BlueOak-1.0.0'],
];

/** 已知用途（写进 README 的「用途」列；未登记的按依赖层级归类）。 */
const PURPOSE = {
  react: 'UI 框架（§12.1 选型）',
  'react-dom': 'UI 渲染',
  scheduler: 'React 运行时依赖',
  '@tauri-apps/api': '前后端 IPC（invoke / event）',
  '@tauri-apps/plugin-dialog': '选择素材文件夹对话框',
  'lucide-react': '图标（§12.4 已批准 Lucide）',
  '@tanstack/react-virtual': '虚拟滚动（§12.4 已批准 TanStack Virtual）',
  '@tanstack/virtual-core': '虚拟滚动内核',
  vite: '构建工具（构建期，不进产物）',
  '@vitejs/plugin-react': 'Vite React 插件（构建期）',
  typescript: 'TypeScript 编译（构建期）',
  '@tauri-apps/cli': 'Tauri 打包 / 开发命令（构建期）',
  '@types/react': 'React 类型定义（构建期）',
  '@types/react-dom': 'ReactDOM 类型定义（构建期）',
};

const args = process.argv.slice(2);
const flag = (name) => args.includes(name);
const opt = (name, fallback) => {
  const i = args.indexOf(name);
  return i >= 0 && args[i + 1] ? args[i + 1] : fallback;
};

/** 求「包含该包的 node_modules 目录」：pnpm 的符号链接必须按 realpath 解析后再上溯。 */
const nodeModulesRoot = (dir, name) => {
  let real = dir;
  try {
    real = realpathSync(dir);
  } catch {
    /* 保持原路径 */
  }
  if (!name) return join(real, 'node_modules');
  const ups = name.startsWith('@') ? 2 : 1; // 作用域包名占两层（@scope/name）
  let up = real;
  for (let i = 0; i < ups; i += 1) up = join(up, '..');
  return up;
};

const resolvePkgDir = (nmRoot, name) => {
  for (const c of [join(nmRoot, name), join(NODE_MODULES, name)]) {
    if (existsSync(join(c, 'package.json'))) return c;
  }
  return null;
};

const readJson = (p) => JSON.parse(readFileSync(p, 'utf8'));

const findLicenseFile = (dir) => {
  if (!existsSync(dir)) return null;
  const hit = readdirSync(dir, { withFileTypes: true }).find(
    (e) => e.isFile() && /^(licen[cs]e|copying)/i.test(e.name),
  );
  return hit ? join(dir, hit.name) : null;
};

const licenseFromText = (file) => {
  const text = readFileSync(file, 'utf8').slice(0, 2000);
  for (const [re, name] of TEXT_SIGNATURES) if (re.test(text)) return name;
  return null;
};

const declaredLicense = (pkg) => {
  if (typeof pkg.license === 'string' && pkg.license.trim()) return pkg.license.trim();
  if (Array.isArray(pkg.licenses) && pkg.licenses.length) {
    return pkg.licenses.map((l) => l.type ?? l).join(' OR ');
  }
  return null;
};

// ── 依赖树 ─────────────────────────────────────────────────────────────
const rootPkg = readJson(join(ROOT, 'package.json'));
const runtime = new Map(); // name → dir（会进发布产物）
const build = new Map(); // name → dir（构建期，不随包分发）

const visit = (nmRoot, name, sink, seen) => {
  const childDir = resolvePkgDir(nmRoot, name);
  if (!childDir) return;
  const pkg = readJson(join(childDir, 'package.json'));
  const key = pkg.name ?? name;
  if (seen.has(key)) return;
  seen.add(key);
  sink.set(key, childDir);
  const childNm = nodeModulesRoot(childDir, key);
  for (const dep of Object.keys(pkg.dependencies ?? {})) visit(childNm, dep, sink, seen);
};

const rootNm = join(ROOT, 'node_modules');
const seenRuntime = new Set();
for (const dep of Object.keys(rootPkg.dependencies ?? {})) visit(rootNm, dep, runtime, seenRuntime);
for (const dep of Object.keys(rootPkg.devDependencies ?? {})) {
  const dir = resolvePkgDir(rootNm, dep);
  if (dir) build.set(dep, dir);
}

// ── 逐包核对 ───────────────────────────────────────────────────────────
const rows = [];
const failures = [];

const audit = (name, dir, scope) => {
  const pkg = readJson(join(dir, 'package.json'));
  const version = pkg.version ?? '未知';
  const rawLicense = declaredLicense(pkg) ?? '（元数据未声明）';
  const file = findLicenseFile(dir);
  const fromText = file ? licenseFromText(file) : null;

  let name_ = rawLicense;
  let verdict = '';
  let note = '';
  let licenseFileName = '—';

  if (!file) {
    verdict = '不允许发布';
    note = '随包找不到 LICENSE / COPYING 文件（§12.4⑤ 禁止凭记忆判定）';
    failures.push(`${name}@${version}：缺少随包 LICENSE 文件`);
  } else {
    licenseFileName = file.slice(dir.length + 1);
    if (fromText) name_ = fromText;
    const spdx = rawLicense.replace(/^\(|\)$/g, '').split(/\s+(?:OR|AND)\s+/)[0];
    if (fromText && rawLicense !== '（元数据未声明）' && fromText !== spdx) {
      note = `元数据写 ${rawLicense}，LICENSE 正文为 ${fromText}（已按正文登记）`;
    }
    const canonical = fromText ?? spdx;
    if (FORBIDDEN.test(canonical)) {
      verdict = '不允许发布';
      note = `${canonical} 与本项目「私有、禁商业、禁再分发」的分发方式冲突`;
      failures.push(`${name}@${version}：${canonical} 不允许本项目分发`);
    } else if (!ALLOWED.has(canonical)) {
      verdict = '需人工判定';
      note = note || `未登记在白名单里的许可证：${canonical}`;
      failures.push(`${name}@${version}：${canonical} 需人工判定`);
    } else {
      verdict = '是';
      if (!fromText) note = note || 'LICENSE 正文未匹配到已知许可证模板，已按元数据登记';
    }
  }

  rows.push({
    name,
    usage: PURPOSE[name] ?? (scope === 'runtime' ? '运行时传递依赖' : '构建期依赖'),
    version,
    file: licenseFileName,
    license: name_,
    verdict,
    scope,
    note,
  });
};

for (const [name, dir] of runtime) audit(name, dir, 'runtime');
for (const [name, dir] of build) audit(name, dir, 'build');

// ── 输出 ───────────────────────────────────────────────────────────────
const by = opt('--by', 'license-audit.mjs');
const date = new Date().toISOString().slice(0, 10);
const sorted = [...rows].sort(
  (a, b) => (a.scope === b.scope ? a.name.localeCompare(b.name) : a.scope === 'runtime' ? -1 : 1),
);

const table = [
  '| 依赖 | 用途 | 版本 | 自带 LICENSE 文件 | 许可证 | 是否允许本项目分发 | 核对人 / 日期 |',
  '| --- | --- | --- | --- | --- | --- | --- |',
  ...sorted.map(
    (r) =>
      `| \`${r.name}\` | ${r.usage} | ${r.version} | ${r.file} | ${r.license} | ${r.verdict} | ${by} / ${date} |`,
  ),
].join('\n');

const notes = sorted.filter((r) => r.note).map((r) => `- \`${r.name}@${r.version}\`：${r.note}`);

if (flag('--readme')) {
  const readmePath = join(ROOT, 'README.md');
  const readme = readFileSync(readmePath, 'utf8');
  const start = '<!-- licenses:start -->';
  const end = '<!-- licenses:end -->';
  const i = readme.indexOf(start);
  const j = readme.indexOf(end);
  if (i < 0 || j < 0 || j < i) {
    console.error(`[licenses] README.md 里找不到 ${start} / ${end} 标记块`);
    process.exit(1);
  }
  const block = [start, '', table, '', ...(notes.length ? ['核对备注：', '', ...notes, ''] : []), end].join('\n');
  writeFileSync(readmePath, readme.slice(0, i) + block + readme.slice(j + end.length), 'utf8');
  console.log(`[licenses] 已写入 README.md（${sorted.length} 个依赖）`);
}

console.log(table);
console.log(
  `\n[licenses] 运行时依赖 ${rows.filter((r) => r.scope === 'runtime').length} 个（会进发布产物）/ 构建期依赖 ${rows.filter((r) => r.scope === 'build').length} 个（不进产物）`,
);
if (failures.length === 0) {
  console.log('[licenses] OK · 全部依赖已按随包 LICENSE 逐版本核对，且允许本项目分发方式（§5 / §12.4⑤）');
  process.exit(0);
}
console.error(`\n[licenses] ${failures.length} 项未通过：`);
for (const f of failures) console.error(`  ${f}`);
process.exit(1);
