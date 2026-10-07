// 校验用：把 web/index.html 的树图布局逻辑（wOf / sizedItems / squarify /
// collectLeaves）原样搬到 Node 里，跑同一棵扫描树，输出与 C++ 侧 TMBAKE 对标的数据。
import fs from 'node:fs';

const mapKind = 'sqrt', ordKind = 'sumG', powAlpha = 0.5;
const LAY_SCALE = 4;
const W = Number(process.argv[2] || 2248), H = Number(process.argv[3] || 2572);

function _G(kind, x) {
  const v = Math.max(Number(x) || 0, 1);
  let r;
  if (kind === 'log') r = Math.log2(v + 2);
  else if (kind === 'log2') r = Math.pow(Math.log2(v + 2), 2);
  else if (kind === 'sqrt') r = Math.sqrt(v);
  else if (kind === 'pow') r = Math.pow(v, (isFinite(powAlpha) ? powAlpha : 0.5));
  else r = v;
  return (isFinite(r) && r > 0) ? r : 1;
}
const cache = new Map();
function _Gsum(n, kind) {
  if (cache.has(n)) return cache.get(n);
  let v;
  if (n.children && n.children.length) {
    v = 0;
    for (const c of n.children) v += _Gsum(c, kind);
    v = Math.max(v, _G(Math.max(n.size, 1)));
  } else v = _G(kind, n.size);
  cache.set(n, v);
  return v;
}
function wOf(n) {
  const s = Math.max(n.size, 1);
  if (mapKind === 'id') return s;
  if (ordKind === 'sumG') return _G(mapKind, s);
  if (ordKind === 'Gsum') return _Gsum(n, mapKind);
  return _G(mapKind, _Gsum(n, mapKind));
}
const sizedItems = arr => arr.map(n => ({ node: n, size: wOf(n) }));

function squarify(items, x, y, w, h, out, depth, maxDepth) {
  if (!items.length || w <= 0 || h <= 0) return;
  const total = items.reduce((s, it) => s + Math.max(it.size, 1), 0);
  let area = w * h;
  if (!items.length) return;
  if (items.length === 1) {
    out.push({ node: items[0].node, x, y, w, h, depth, frac: Math.max(items[0].size, 1) / total });
    return;
  }
  if (w <= 0.01 || h <= 0.01) {
    const sum = items.reduce((s, it) => s + Math.max(it.size, 1), 0) || 1;
    let cx = x, cy = y, cw = w, ch = h;
    const horiz = w >= h;
    for (let k = 0; k < items.length; k++) {
      const it = items[k];
      const f = Math.max(it.size, 1) / sum;
      if (k === items.length - 1) out.push({ node: it.node, x: cx, y: cy, w: cw, h: ch, depth, frac: f });
      else if (horiz) { const sw = cw * f; out.push({ node: it.node, x: cx, y: cy, w: sw, h: ch, depth, frac: f }); cx += sw; cw -= sw; }
      else { const sh = ch * f; out.push({ node: it.node, x: cx, y: cy, w: cw, h: sh, depth, frac: f }); cy += sh; ch -= sh; }
    }
    return;
  }
  const row = [];
  let rowArea = 0;
  const worst = (r, side) => {
    const sum = r.reduce((s, i) => s + Math.max(i.size, 1), 0) / total * area;
    if (!sum || !side) return Infinity;
    let mx = 0, mn = Infinity;
    for (const i of r) { const a = Math.max(i.size, 1) / total * area; mx = Math.max(mx, a); mn = Math.min(mn, a); }
    const s2 = sum * sum, side2_ = side * side;
    return Math.max(side2_ * mx / s2, s2 / (side2_ * mn));
  };
  const shortSide = Math.min(w, h);
  let i = 0;
  const remaining = [...items];
  while (i < remaining.length) {
    const it = remaining[i];
    const cur = row.concat([it]);
    if (row.length === 0 || worst(cur, shortSide) <= worst(row, shortSide)) { row.push(it); rowArea += Math.max(it.size, 1); i++; }
    else break;
  }
  if (row.length === 0) { row.push(remaining[0]); rowArea = Math.max(remaining[0].size, 1); i = 1; }
  const frac = rowArea / total;
  const isWide = w >= h;
  if (isWide) {
    const rw = area * frac / h;
    let cy = y;
    for (const it of row) {
      const ih = h * (Math.max(it.size, 1) / rowArea);
      out.push({ node: it.node, x, y: cy, w: rw, h: ih, depth, frac: Math.max(it.size, 1) / total });
      cy += ih;
    }
    squarify(remaining.slice(i), x + rw, y, w - rw, h, out, depth, maxDepth);
  } else {
    const rh = area * frac / w;
    let cx = x;
    for (const it of row) {
      const iw = w * (Math.max(it.size, 1) / rowArea);
      out.push({ node: it.node, x: cx, y, w: iw, h: rh, depth, frac: Math.max(it.size, 1) / total });
      cx += iw;
    }
    squarify(remaining.slice(i), x, y + rh, w, h - rh, out, depth, maxDepth);
  }
}

const leaves = [], dirs = [];
function collectLeaves(items, x, y, w, h, depth, anc, depthCap) {
  if (!items.length || w < 0.01 || h < 0.01) return;
  const cap = (depthCap == null) ? Infinity : depthCap;
  const placed = [];
  squarify(sizedItems(items), x, y, w, h, placed, 0, 1);
  for (const p of placed) {
    const n = p.node;
    const sub = (n.isDir && n.children && n.children.length) ? n.children : null;
    const pw = Math.round(p.w), ph = Math.round(p.h);
    if (n.isDir && pw >= 12 && ph >= 12) dirs.push({ node: n, depth });
    if (sub && pw >= 0.02 && ph >= 0.02 && depth < cap) collectLeaves(sub, p.x, p.y, p.w, p.h, depth + 1, anc.concat([n.id]), cap);
    else leaves.push({ node: n, x: p.x, y: p.y, w: p.w, h: p.h, depth });
  }
}

// 递归排序：目录优先 + size 降序（宿主/后端保证的顺序）
let idSeq = 1;
function normalize(n) {
  n.id = idSeq++;
  if (n.children) {
    for (const c of n.children) normalize(c);
    n.children.sort((a, b) => (a.isDir !== b.isDir) ? (a.isDir ? -1 : 1) : (b.size - a.size));
  }
}

const j = JSON.parse(fs.readFileSync(process.argv[4] || 'D:/Desktop/diskmate/temp/tree.json', 'utf8'));
const root = j.roots[0];
normalize(root);
collectLeaves(root.children, 0, 0, W, H, 0, [root.id], 999);
const top = [...leaves].sort((a, b) => (b.w * b.h) - (a.w * a.h)).slice(0, 5);
console.log(JSON.stringify({
  cur: root.name, allLeaves: leaves.length, allDirs: dirs.length, W, H,
  top: top.map(r => `${r.node.name}=${r.node.size}B/${Math.round(r.w * r.h)}px2`),
}, null, 1));
