// 列出文档中每一条「第X章 / 第X节」引用及其解析到的章标题，便于人工核对语义是否对得上
const fs = require('fs');
const t = fs.readFileSync(process.argv[2], 'utf8');
const lines = t.split(/\r?\n/);
const cn = ['一', '二', '三', '四', '五', '六', '七', '八', '九', '十', '十一', '十二', '十三', '十四', '十五', '十六', '十七', '十八', '十九', '二十'];
const title = {};
lines.forEach((l) => {
  const m = /^## ([一二三四五六七八九十]+)、(.+)$/.exec(l.trim());
  if (m) title[m[1]] = m[2];
});
const idx = {};
cn.forEach((c, i) => { idx[c] = i; });
let out = 0;
lines.forEach((l, i) => {
  const re = /第([一二三四五六七八九十]+)([章节])/g;
  let m;
  while ((m = re.exec(l)) !== null) {
    const n = m[1];
    const target = title[n] || '???';
    const num = idx[n] === undefined ? '?' : idx[n] + 1;
    // 句子片段：截取引用前 26 字
    const pre = l.slice(Math.max(0, m.index - 26), m.index).replace(/[|*]/g, '');
    console.log('L' + (i + 1) + '  ' + m[0] + ' (第' + num + '章→' + target + ')   …' + pre);
    out++;
  }
});
console.log('--- total refs = ' + out);
