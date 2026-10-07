// 验证 web 版命中测试改动：把 index.html 里的**真实函数**抽出来跑
//   - 新实现 tmHitTest（沿树下行的递归）
//   - 旧实现 tmHitTestScan（线性扫描，留作对照）
// 用同一棵合成大树 + 同一套布局，逐点比对两者结果，并对比耗时。
// 用法： node temp/verify_hit.mjs [叶子目标数量]
import fs from 'node:fs';

const html = fs.readFileSync(new URL('../web/index.html', import.meta.url), 'utf8');
const lines = html.split(/\r?\n/);

function extract(name) {
  const re = new RegExp('^(\\s*)function ' + name + '\\b');
  const i = lines.findIndex(l => re.test(l));
  if (i < 0) throw new Error('找不到函数: ' + name);
  const indent = lines[i].match(/^(\s*)/)[1];
  if (lines[i].trimEnd().endsWith('}')) return lines[i];   // 单行函数
  for (let j = i + 1; j < lines.length; j++) {
    if (lines[j] === indent + '}') return lines.slice(i, j + 1).join('\n');
    if (/^\s*function\s/.test(lines[j])) break;            // 防越界：撞到下一个函数就停
  }
  throw new Error('函数没有结束: ' + name);
}

const names = ['_G', '_Gsum', 'wOf', 'clearWeightCache', 'sizedItems', 'packItems', 'squarify',
               'collectLeaves', 'vx', 'vy', 'tmRect', 'nodeBlockValid', 'tmRectOfNode',
               'nodeCanvasRect', 'hitDeepest', 'tmHitTest', 'tmHitTestScan', 'unionRectOf', 'findNodeById'];
let code = '';
const found = [];
for (const n of names) {
  try { code += extract(n) + '\n'; found.push(n); } catch { /* 可选函数缺失不算错 */ }
}
console.log('抽出函数:', found.join(', '));

const prelude = `
let _wCache = new WeakMap();
let mapKind = 'sqrt', ordKind = 'sumG', powAlpha = 0.5;
const LAY_SCALE = 4;
const TM_W = 848, TM_H = 996;
let tmBlockGen = 0;
let tmAllLeaves = [], tmAllDirs = [], tmLiveLeaves = [];
let tmView = { k: 1 / LAY_SCALE, tx: 0, ty: 0 };   // 与线上一致：box=整块虚拟画布 → k = dW/(W*LAY_SCALE)
const state = { root: null, current: null, _treeById: new Map() };
const post = () => {};
function weightKey() { return 'k'; }
function ensureCanvas() { return { W: TM_W, H: TM_H }; }
function clearWeightCache2() {}
`;
const epilogue = `
export { tmHitTest, tmHitTestScan, tmRect, tmRectOfNode, unionRectOf, collectLeaves,
         tmAllLeaves, tmLiveLeaves, tmAllDirs, tmBlockGen, state, TM_W, TM_H, LAY_SCALE, tmView,
         vx, vy, bumpGen, setLive, getLeaves, setCurrent };
`;
const mod = prelude + code +
  'function getLeaves(){ return tmAllLeaves; }\n' +
  'function setLive(){ tmLiveLeaves = tmAllLeaves; }\n' +
  'function bumpGen(){ tmBlockGen++; }\n' +
  'function setCurrent(n){ state.current = n; state.root = n; }\n' +
  epilogue;

fs.writeFileSync(new URL('./_hitmod.mjs', import.meta.url), mod);
const M = await import('./_hitmod.mjs');

// ---------- 造一棵像真实扫描结果的大树 ----------
const target = Number(process.argv[2] || 120000);
let nextId = 1, nodes = 0;
const rnd = (() => { let s = 12345; return () => (s = (s * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff; })();
const EXTS = ['bin', 'js', 'json', 'zip', 'mp4', 'png', 'dll', 'txt', 'log', 'dat'];

function mkDir(name, budget, depth) {
  const n = { id: nextId++, name, size: 0, isDir: true, children: [] };
  // 目录优先 + 大小降序（C++ 端保证的顺序，这里手工模拟）
  const nk = depth === 0 ? 40 : Math.max(1, Math.min(40, Math.round(1 + rnd() * (budget > 2000 ? 30 : 6))));
  const subs = [];
  for (let i = 0; i < nk; i++) {
    const isDir = depth < 6 && rnd() < 0.25 && budget > 200;
    if (isDir) { const c = mkDir(name + '/d' + i, budget / nk, depth + 1); subs.push(c); }
    else {
      const c = { id: nextId++, name: name + '/f' + i + '.' + EXTS[(rnd() * EXTS.length) | 0],
                  size: Math.round(1 + rnd() * (budget / 3)), isDir: false, children: null, parent: n };
      subs.push(c); nodes++;
    }
    if (nodes >= target) break;
  }
  subs.sort((a, b) => (b.isDir - a.isDir) || (b.size - a.size));
  for (const c of subs) c.parent = n;
  n.children = subs;
  n.size = subs.reduce((s, c) => s + c.size, 1);
  return n;
}
const root = mkDir('root', 3.2e9, 0);
root.parent = null;
const index = new Map();
(function walk(n) { index.set(n.id, n); if (n.children) for (const c of n.children) walk(c); })(root);
M.state._treeById = index;
M.state.root = root; M.state.current = root;

// ---------- 布局（和线上一致：4× 虚拟画布，depthCap=999）----------
const W = M.TM_W * M.LAY_SCALE, H = M.TM_H * M.LAY_SCALE;
M.bumpGen();
M.tmAllLeaves.length = 0;
M.collectLeaves(root.children, 0, 0, W, H, M.tmAllLeaves, 0, [root.id], 999);
M.setLive();
const allLeaves = M.tmAllLeaves;

console.log(`树: ${nodes} 个节点 / 块(叶子) ${allLeaves.length} 个  画布虚拟 ${W}x${H}`);

// ---------- 1) 逐点比对 ----------
let sample = 0, mismatch = 0, first = null;
const cmp = (x, y) => {
  sample++;
  const a = M.tmHitTest(x, y), b = M.tmHitTestScan(x, y);
  const ai = a ? a.id : null, bi = b ? b.id : null;
  if (ai !== bi) { mismatch++; if (!first) first = { x: Math.round(x), y: Math.round(y), fast: ai, scan: bi }; }
};
for (let y = 0; y < M.TM_H; y += 3)
  for (let x = 0; x < M.TM_W; x += 3) cmp(x + 0.5, y + 0.5);
const step = Math.max(1, (allLeaves.length / 3000) | 0);
for (let i = 0; i < allLeaves.length; i += step) {
  const r = allLeaves[i];
  cmp(Math.floor(M.vx(r.x + r.w / 2)), Math.floor(M.vy(r.y + r.h / 2)));
}
for (let i = 0; i < 5000; i++) cmp(rnd() * M.TM_W, rnd() * M.TM_H);

console.log(`\n【命中对照】采样 ${sample} 点 → 不一致 ${mismatch} 处`);
if (first) console.log('  首个不一致:', JSON.stringify(first));

// ---------- 2) 性能 ----------
const pts = [];
for (let i = 0; i < 2000; i++) pts.push([rnd() * M.TM_W, rnd() * M.TM_H]);
const timeIt = (f, reps) => {
  const t = performance.now();
  for (let r = 0; r < reps; r++) for (const [x, y] of pts) f(x, y);
  return performance.now() - t;
};
const fast = timeIt(M.tmHitTest, 1), scan = timeIt(M.tmHitTestScan, 1);
console.log(`【性能】2000 次命中:  新(递归下行) ${fast.toFixed(1)} ms   旧(线性扫描) ${scan.toFixed(1)} ms   加速 ${(scan / fast).toFixed(0)}×`);

// ---------- 3) 并集框等价性（新：节点自身块；旧：后代叶子并集）----------
function oldUnion(id) {
  let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity, hit = false;
  for (const r of allLeaves) {
    let anc = false;
    for (let p = r.node.parent; p; p = p.parent) if (p.id === id) { anc = true; break; }
    if (r.node.id !== id && !anc) continue;
    const R = M.tmRect(r);
    x0 = Math.min(x0, R.x0); y0 = Math.min(y0, R.y0);
    x1 = Math.max(x1, R.x1); y1 = Math.max(y1, R.y1); hit = true;
  }
  return hit ? { x0, y0, x1, y1 } : null;
}
let dirs = 0, nullDiff = 0, pxDiff = 0, worst = 0, nullSample = null;
(function walk(n) {
  if (!n.isDir || !n.children.length) { if (n.children) for (const c of n.children) walk(c); return; }
  const a = M.unionRectOf(n.id), b = oldUnion(n.id);
  dirs++;
  if ((!!a) !== (!!b)) {
    // 一边 null：只会发生在「块在画布上不足 1px」的情况（新实现返回 null，旧的返回 <1px 矩形）
    // drawHl 本来就有 `R.w<1 || R.h<1 → 不画` 的保护，结果等价
    nullDiff++;
    if (!nullSample) {
      const r = a || b;
      nullSample = { id: n.id, w: r ? r.x1 - r.x0 : 0, h: r ? r.y1 - r.y0 : 0,
                     aNull: !a, bNull: !b, kids: n.children.length };
    }
  } else if (a && b) {
    const d = Math.max(Math.abs(a.x0 - b.x0), Math.abs(a.y0 - b.y0), Math.abs(a.x1 - b.x1), Math.abs(a.y1 - b.y1));
    if (d > worst) worst = d;
    if (d > 1) pxDiff++;
  }
  for (const c of n.children) walk(c);
})(root);
console.log(`【并集框】检查 ${dirs} 个目录 → 像素差>1px: ${pxDiff} 处（最大偏差 ${worst}px）；` +
            `null/非null: ${nullDiff} 处（应全部是画布上 <1px 的块）`);
if (nullSample) console.log('  样例:', JSON.stringify(nullSample));
const ok = mismatch === 0 && pxDiff === 0;
console.log(ok ? '\n结果: 通过 ✅（命中逐点一致；并集框只有 ≤1px 取整差）' : '\n结果: 有差异 ❌');
