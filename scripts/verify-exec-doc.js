// 执行版文档结构自检：章号连续、小节号存在、交叉引用可解析、工作量分项相加
const fs = require('fs');
const p = process.argv[2];
const t = fs.readFileSync(p, 'utf8');
const lines = t.split(/\r?\n/);

const cn = ['一', '二', '三', '四', '五', '六', '七', '八', '九', '十', '十一', '十二', '十三', '十四', '十五', '十六', '十七', '十八', '十九'];
const chapters = [];
lines.forEach((l, i) => { const m = /^## ([一二三四五六七八九十]+)、/.exec(l); if (m) chapters.push({ n: m[1], line: i + 1, text: l.trim() }); });

console.log('章数 = ' + chapters.length);
let ok = true;
chapters.forEach((c, idx) => {
  if (c.n !== cn[idx]) { console.log('X 章号不连续: 第 ' + (idx + 1) + ' 个是「' + c.n + '」应为「' + cn[idx] + '」 @' + c.line); ok = false; }
});
console.log(ok ? 'OK 章号 一…' + cn[chapters.length - 1] + ' 连续无重复' : 'FAIL 章号异常');

// 小节标题集合
const secs = new Set();
lines.forEach((l) => { const m = /^#{3,4} (\d+(?:\.\d+)*)/.exec(l); if (m) secs.add(m[1]); });
console.log('小节标题数 = ' + secs.size + ' → ' + [...secs].join(' '));

// 交叉引用可解析性
const refRe = /(?:见|参见|详见)\s*(\d+(?:\.\d+)*)|[（(](\d+(?:\.\d+)*)[）)]|第\s*(\d+(?:\.\d+)*)\s*节/g;
const bad = new Map();
let m;
while ((m = refRe.exec(t)) !== null) {
  const v = m[1] || m[2] || m[3];
  if (!v) continue;
  if (!secs.has(v)) { bad.set(v, (bad.get(v) || 0) + 1); }
}
if (bad.size === 0) console.log('OK 交叉引用（见 X.Y / （X.Y） / 第 X.Y 节）全部能解析到小节标题');
else { console.log('需人工确认的引用（可能是数值而非引用）：'); [...bad.entries()].sort().forEach(([k, c]) => console.log('  ' + k + ' ×' + c)); }

// 工作量分项
const rows = [...t.matchAll(/^\| P(\d)[^|]*\|[^|]*\|[^|]*\|\s*\*{0,2}([\d.]+)\*{0,2}\s*\|$/gm)].map((x) => [Number(x[1]), Number(x[2])]);
if (rows.length) {
  const sum = rows.reduce((a, b) => a + b[1], 0);
  console.log('分项 = ' + rows.map((r) => 'P' + r[0] + ':' + r[1]).join(' ') + '  相加 = ' + sum.toFixed(1));
  const claim = /合计[^0-9]{0,8}\*{0,2}([\d.]+)\s*人日/.exec(t);
  console.log('文中合计 = ' + (claim ? claim[1] : '未找到') + (claim && Number(claim[1]) === Number(sum.toFixed(1)) ? '  ✓ 一致' : '  ✗ 不一致'));
}
