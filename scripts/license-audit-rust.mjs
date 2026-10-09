/**
 * Rust 依赖许可证逐版本核对（执行版 §5 / §12.4⑤）。
 *
 * 与 `license-audit.mjs`（npm 侧）配套：npm 侧核对 `node_modules` 里每个包随包的 LICENSE；
 * 这里核对 **Cargo 编译进二进制的每一颗 crate**——走 `cargo metadata`，逐版本看
 * ①声明的许可证 ②该版本目录下是否真的带着 LICENSE 文件。
 *
 * 用法：
 *   node scripts/license-audit-rust.mjs            只核对，打印摘要
 *   node scripts/license-audit-rust.mjs --list     打印完整表格（markdown）
 *   node scripts/license-audit-rust.mjs --write    把完整表格写进 docs/依赖许可证-Rust.md
 *
 * 退出码：0 全部合规；1 出现「未声明许可证」或「非宽松许可证」。
 * 说明：缺失 LICENSE 文件只记为提醒（很多 crate 只在 SPDX 字段声明、不随包放文件），
 * 这属于「需人工确认」，不会让门禁失败——门禁失败留给真正的风险项。
 */

import { execFileSync } from 'node:child_process';
import { existsSync, readdirSync, writeFileSync, mkdirSync } from 'node:fs';
import { dirname, join, basename } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, '..');
const MANIFEST = join(ROOT, 'src-tauri', 'Cargo.toml');
const OUT_DOC = join(ROOT, 'docs', '依赖许可证-Rust.md');

const args = process.argv.slice(2);
const wantList = args.includes('--list');
const wantWrite = args.includes('--write');

/** 允许的宽松许可证族（§5：本项目为源码可见的专有软件，随包依赖须为宽松许可）。 */
const PERMISSIVE = [
  'MIT',
  'MIT-0',
  'Apache-2.0',
  'BSD-2-Clause',
  'BSD-3-Clause',
  'ISC',
  'Zlib',
  'CC0-1.0',
  'Unlicense',
  '0BSD',
  'MPL-2.0',
  'Unicode-3.0',
  'Unicode-DFS-2016',
  'BSL-1.0',
];

/** SPDX 例外后缀：附加在 WITH 之后，不单独构成一个许可证。 */
const SPDX_EXCEPTIONS = new Set([
  'LLVM-exception',
  'Autoconf-exception',
  'Bison-exception',
  'GCC-exception',
  'Classpath-exception-2.0',
  'OpenSSL-exception',
  'Font-exception-2.0',
  'Linux-syscall-note',
  'Universal-FOSS-exception-1.0',
  'mif-exception',
  'Bootloader-exception',
  '389-exception',
]);

function readMetadata() {
  const raw = execFileSync(
    'cargo',
    ['metadata', '--format-version', '1', '--manifest-path', MANIFEST],
    { encoding: 'utf8', maxBuffer: 128 * 1024 * 1024 },
  );
  return JSON.parse(raw);
}

function licenseFiles(dir) {
  if (!existsSync(dir)) return [];
  try {
    return readdirSync(dir).filter((f) => /^(LICEN[CS]E|COPYING|NOTICE)/i.test(f));
  } catch {
    return [];
  }
}

/** 一段（AND 组内）的许可证标识集合：拆掉括号、斜杠、WITH 与其例外后缀。 */
function tokensOf(part) {
  return part
    .split(/[()/]|\s+WITH\s+/i)
    .map((s) => s.trim())
    .filter((s) => s && !SPDX_EXCEPTIONS.has(s));
}

/**
 * 按 SPDX 语义判断是否可以用宽松许可：
 * - `A OR B`：**任一**分支可用即可（用户可选）；
 * - `A AND B`：**每个**都必须可用；
 * - `MIT/Apache-2.0` 是 `MIT OR Apache-2.0` 的等价写法（Cargo 生态常见）。
 */
function isPermissive(expr) {
  const groups = expr.split(/\s+OR\s+/i);
  for (const group of groups) {
    const andParts = group.split(/\s+AND\s+/i);
    const ok = andParts.every((part) => {
      const tokens = tokensOf(part);
      return tokens.length > 0 && tokens.every((t) => PERMISSIVE.includes(t));
    });
    if (ok) return true;
  }
  return false;
}

function main() {
  const meta = readMetadata();
  const own = new Set(meta.workspace_members ?? []);
  const rows = [];

  for (const pkg of meta.packages) {
    if (own.has(pkg.id)) continue;
    const dir = dirname(pkg.manifest_path);
    const declared = (pkg.license ?? '').trim();
    const files = licenseFiles(dir);
    const permissive = isPermissive(declared);
    rows.push({
      name: pkg.name,
      version: pkg.version,
      declared: declared || (pkg.license_file ? `文件 ${basename(pkg.license_file)}` : ''),
      files,
      permissive,
    });
  }

  rows.sort((a, b) => a.name.localeCompare(b.name) || a.version.localeCompare(b.version));

  const noDecl = rows.filter((r) => !r.declared);
  const notPermissive = rows.filter((r) => r.declared && !r.permissive);
  const noFile = rows.filter((r) => r.declared && r.files.length === 0);

  console.log(`[license-rust] 随包 crate ${rows.length} 个（按版本逐条核对）`);
  console.log(`[license-rust] 未声明许可证：${noDecl.length}`);
  console.log(`[license-rust] 非宽松许可证：${notPermissive.length}`);
  console.log(`[license-rust] 目录内无 LICENSE 文件（提醒，不阻断）：${noFile.length}`);

  for (const r of noDecl) console.log(`  ✗ 未声明：${r.name}@${r.version}`);
  for (const r of notPermissive) console.log(`  ✗ 非宽松：${r.name}@${r.version} → ${r.declared}`);
  for (const r of noFile.slice(0, 8)) console.log(`  · 无文件：${r.name}@${r.version}（SPDX 已声明 ${r.declared}）`);
  if (noFile.length > 8) console.log(`  · …另有 ${noFile.length - 8} 个同样只有 SPDX 声明`);

  if (wantList || wantWrite) {
    const table = [
      '| crate | 版本 | 声明许可证 | 目录内 LICENSE |',
      '| --- | --- | --- | --- |',
      ...rows.map(
        (r) => `| ${r.name} | ${r.version} | ${r.declared || '（未声明）'} | ${r.files.join('、') || '—'} |`,
      ),
    ].join('\n');
    if (wantWrite) {
      mkdirSync(dirname(OUT_DOC), { recursive: true });
      writeFileSync(
        OUT_DOC,
        [
          '# Rust 依赖许可证清单（随包编译进二进制的每一颗 crate）',
          '',
          '由 `node scripts/license-audit-rust.mjs --write` 生成（执行版 §5 / §12.4⑤）。',
          '修改依赖后请重新生成，不要手改本文件。',
          '',
          table,
          '',
        ].join('\n'),
        'utf8',
      );
      console.log(`[license-rust] 已写入 docs/${basename(OUT_DOC)}`);
    } else {
      console.log(table);
    }
  }

  if (noDecl.length > 0 || notPermissive.length > 0) {
    console.log('[license-rust] 结果：不合规（见上）');
    process.exit(1);
  }
  console.log('[license-rust] OK · 全部为宽松许可证且逐版本已核对');
}

main();
