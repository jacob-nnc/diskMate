// ============================================================================
// DiskMate ImGui 前端 —— 右侧树图
//   算法与观感逐项移植 web/index.html：
//     · 4× 超采样虚拟画布（LAY_SCALE=4），布局后整体 ×0.25 映射到画布
//     · squarify（worst 比例判据 + 短边切行）与 collectLeaves（0.02px 递归阈值、
//       12px 目录外框阈值）精确复刻
//     · 面积权重 wOf：映射 g × 运算顺序（与 HTML _G / _Gsum / wOf 一致）
//     · 片元着色器对角渐变 → 四角顶点色等价实现
//     · 目录外框只画最外层；文字 clip + 省略号；焦点框 + 信息面板一体
//     · 滚轮逐级父级框选（会话基块 + 500ms 防误触）
//   HTML 里 maxDepth/minSizeMB/maxRects/kidsCap/colorScheme/showLabels 这些设置项
//   对树图【不生效】（后端传什么就画什么），这里同样不生效，保持两侧一致。
// ============================================================================
#include "dm.h"
#include "imgui_internal.h"

#include <d3d11.h>

using namespace dm;

// ---------------------------------------------------------------- 状态 ----
namespace {

struct SizedItem { ScanNode* node; double size; };
struct Placed    { ScanNode* node; double x, y, w, h; };
struct Leaf      { ScanNode* node; double x, y, w, h; int depth; };
struct DirRect   { ScanNode* node; double x, y, w, h; int depth; };
struct WheelEnt  { ScanNode* id; bool has; double x0, y0, x1, y1; };

std::vector<Leaf>    s_leaves;
std::vector<DirRect> s_dirs;

// 顶点/索引烘焙缓存（布局不变时每帧只做一次 memcpy / 变换拷贝）
std::vector<ImDrawVert> s_vtx;
std::vector<ImDrawIdx>  s_idx;
std::vector<const Leaf*>    s_labelLeaves;   // 名称+大小文字
std::vector<const DirRect*> s_frameDirs;     // 最外层目录外框
std::vector<const Leaf*>    s_rimLeaves;     // 需要 1px 边缘浮雕的大块

// 布局缓存键
ScanNode* s_keyNode = nullptr;
float     s_keyW = 0, s_keyH = 0;
int       s_keyStamp = -1, s_keyTheme = -1;
size_t    s_keyExtHash = 0;
double    s_layoutMs = 0.0;

// 悬停 / 命中
Leaf*     s_hoverLeaf = nullptr;
ScanNode* s_hoverNode = nullptr;
ImVec2    s_lastMouse{ -1e9f, -1e9f };
bool      s_mouseValid = false;

// 滚轮框选
std::vector<WheelEnt> s_wheelStack;
ScanNode* s_wheelId = nullptr;
bool      s_wheelHas = false;
double    s_wheelX0 = 0, s_wheelY0 = 0, s_wheelX1 = 0, s_wheelY1 = 0;
ScanNode* s_wheelBaseHover = nullptr;
double    s_lastWheelTick = -1e9;

// 面板位置（本帧）
ImVec2 s_panelPos{ 0, 0 };
float  s_panelW = 0, s_panelH = 0;

}  // namespace

// ---------------------------------------------------------------- 权重 ----
// HTML _G(kind, x)：单值变换 g，结果必须有限且 > 0
static double G_(const AppSettings& st, double x) {
    double v = std::max(x, 1.0);
    double r;
    switch (st.mapKind) {
        case 1: r = log2(v + 2.0); break;
        case 2: { double l = log2(v + 2.0); r = l * l; } break;
        case 3: r = sqrt(v); break;
        case 4: { double a = st.powAlpha; if (!(a > 0.01)) a = 0.5; r = pow(v, a); } break;
        default: r = v; break;   // 0 = id
    }
    return (std::isfinite(r) && r > 0) ? r : 1.0;
}

// HTML _Gsum(n, kind)：目录 = Σ g(每个后代文件)，兜底不小于自身的 g
static double GsumOf(ScanNode* n, const AppSettings& st,
                     std::unordered_map<ScanNode*, double>& memo) {
    auto it = memo.find(n);
    if (it != memo.end()) return it->second;
    double v;
    if (!n->children.empty()) {
        v = 0;
        for (ScanNode* c : n->children) v += GsumOf(c, st, memo);
        v = std::max(v, G_(st, std::max((double)n->size, 1.0)));
    } else {
        v = G_(st, (double)n->size);
    }
    memo[n] = v;
    return v;
}

// HTML wOf(n)
static double WeightOf(ScanNode* n, const AppSettings& st,
                       std::unordered_map<ScanNode*, double>& memo) {
    double s = std::max((double)n->size, 1.0);
    if (st.mapKind == 0) return s;                       // id：两种顺序同结果
    if (st.ordKind == 0) return G_(st, s);               // sumG：g(Σx)
    double gs = GsumOf(n, st, memo);
    if (st.ordKind == 1) return gs;                      // Gsum：Σg(leaf)
    return G_(st, gs);                                   // g(Σg(leaf))
}

// -------------------------------------------------------------- squarify ----
// HTML squarify()：逐行放入直到长宽比变差，沿短边铺一条，剩余区域递归
static void Squarify(const std::vector<SizedItem>& items, double x, double y, double w, double h,
                     std::vector<Placed>& out) {
    if (items.empty() || w <= 0 || h <= 0) return;
    double total = 0;
    for (const auto& it : items) total += std::max(it.size, 1.0);
    double area = w * h;

    if (items.size() == 1) {
        out.push_back({ items[0].node, x, y, w, h });
        return;
    }
    if (w <= 0.01 || h <= 0.01) {
        double sum = 0;
        for (const auto& it : items) sum += std::max(it.size, 1.0);
        if (sum <= 0) sum = 1;
        double cx = x, cy = y, cw = w, ch = h;
        bool horiz = (w >= h);
        for (size_t k = 0; k < items.size(); k++) {
            double f = std::max(items[k].size, 1.0) / sum;
            if (k + 1 == items.size()) {
                out.push_back({ items[k].node, cx, cy, cw, ch });
            } else if (horiz) {
                double sw = cw * f;
                out.push_back({ items[k].node, cx, cy, sw, ch });
                cx += sw; cw -= sw;
            } else {
                double sh = ch * f;
                out.push_back({ items[k].node, cx, cy, cw, sh });
                cy += sh; ch -= sh;
            }
        }
        return;
    }

    // 标准 squarify 的 worst ratio
    auto worst = [&](const std::vector<SizedItem>& r, double side) -> double {
        double sum = 0;
        for (const auto& i : r) sum += std::max(i.size, 1.0);
        sum = sum / total * area;
        if (!(sum > 0) || !(side > 0)) return 1e18;      // HTML: Infinity
        double mx = 0, mn = 1e18;
        for (const auto& i : r) {
            double a = std::max(i.size, 1.0) / total * area;
            mx = std::max(mx, a);
            mn = std::min(mn, a);
        }
        double s2 = sum * sum, side2 = side * side;
        if (!(s2 > 0) || !(mn > 0)) return 1e18;
        return std::max(side2 * mx / s2, s2 / (side2 * mn));
    };

    const double shortSide = std::min(w, h);
    std::vector<SizedItem> row;
    double rowArea = 0;
    size_t i = 0;
    while (i < items.size()) {
        std::vector<SizedItem> cur = row;
        cur.push_back(items[i]);
        if (row.empty() || worst(cur, shortSide) <= worst(row, shortSide)) {
            row.push_back(items[i]);
            rowArea += std::max(items[i].size, 1.0);
            i++;
        } else {
            break;
        }
    }
    if (row.empty()) {
        row.push_back(items[0]);
        rowArea = std::max(items[0].size, 1.0);
        i = 1;
    }

    double frac = rowArea / total;
    bool isWide = (w >= h);
    if (isWide) {
        double rw = area * frac / h;
        double cy = y;
        for (const auto& it : row) {
            double ih = h * (std::max(it.size, 1.0) / rowArea);
            out.push_back({ it.node, x, cy, rw, ih });
            cy += ih;
        }
        std::vector<SizedItem> rest(items.begin() + (ptrdiff_t)i, items.end());
        Squarify(rest, x + rw, y, w - rw, h, out);
    } else {
        double rh = area * frac / w;
        double cx = x;
        for (const auto& it : row) {
            double iw = w * (std::max(it.size, 1.0) / rowArea);
            out.push_back({ it.node, cx, y, iw, rh });
            cx += iw;
        }
        std::vector<SizedItem> rest(items.begin() + (ptrdiff_t)i, items.end());
        Squarify(rest, x, y + rh, w, h - rh, out);
    }
}

// ---------------------------------------------------------- 递归收集叶子 ----
static void CollectLeaves(const std::vector<ScanNode*>& items, double x, double y, double w, double h,
                          int depth, double cap, const AppSettings& st,
                          std::unordered_map<ScanNode*, double>& memo,
                          std::vector<Leaf>& leaves, std::vector<DirRect>& dirs) {
    if (items.empty() || w < 0.01 || h < 0.01) return;
    std::vector<SizedItem> sized;
    sized.reserve(items.size());
    for (ScanNode* n : items) sized.push_back({ n, WeightOf(n, st, memo) });

    std::vector<Placed> placed;
    size_t est = sized.size() < 64 ? sized.size() : 64;
    placed.reserve(est);
    Squarify(sized, x, y, w, h, placed);

    for (const auto& p : placed) {
        ScanNode* n = p.node;
        bool hasSub = n->isDir && !n->children.empty();
        double pw = std::floor(p.w + 0.5), ph = std::floor(p.h + 0.5);   // JS Math.round
        if (n->isDir && pw >= 12 && ph >= 12) dirs.push_back({ n, p.x, p.y, p.w, p.h, depth });
        if (hasSub && pw >= 0.02 && ph >= 0.02 && (double)depth < cap) {
            CollectLeaves(n->children, p.x, p.y, p.w, p.h, depth + 1, cap, st, memo, leaves, dirs);
        } else {
            leaves.push_back({ n, p.x, p.y, p.w, p.h, depth });
        }
    }
}

// -------------------------------------------------------------- 顶点烘焙 ----
static inline ImU32 LitRgb(float r, float g, float b, float amt) {
    auto L = [&](float c) {
        float v = c + amt;
        v = v < 0 ? 0.0f : (v > 1.0f ? 1.0f : v);
        return (int)(v * 255.0f + 0.5f);
    };
    return IM_COL32(L(r), L(g), L(b), 255);
}
static inline void Rgb01(ImU32 c, float& r, float& g, float& b) {
    r = ((c >> IM_COL32_R_SHIFT) & 255) / 255.0f;
    g = ((c >> IM_COL32_G_SHIFT) & 255) / 255.0f;
    b = ((c >> IM_COL32_B_SHIFT) & 255) / 255.0f;
}

// 4× 虚拟坐标 → 画布坐标
static inline float VX(double x) { return (float)(x / (double)LAYOUT_SCALE); }

// HTML tmRect()：floor/ceil + 半开区间（命中与高亮共用，保证点哪儿=框哪儿）
static inline void TmRect(const Leaf& r, float& x0, float& y0, float& x1, float& y1) {
    x0 = floorf(VX(r.x));
    y0 = floorf(VX(r.y));
    x1 = ceilf(VX(r.x + r.w));
    y1 = ceilf(VX(r.y + r.h));
}

static void BakeAll() {
    s_vtx.clear(); s_idx.clear(); s_labelLeaves.clear(); s_frameDirs.clear(); s_rimLeaves.clear();
    const bool dark = (g_st.theme != 1);

    s_vtx.reserve(s_leaves.size() * 4 + 1024);
    s_idx.reserve(s_leaves.size() * 6 + 1536);

    for (const Leaf& L : s_leaves) {
        float x0 = VX(L.x), y0 = VX(L.y);
        float x1 = VX(L.x + L.w), y1 = VX(L.y + L.h);
        float sw = x1 - x0, sh = y1 - y0;
        if (sw < 0.05f || sh < 0.05f) continue;

        ImU32 base = L.node->isDir ? ColTmDir() : ExtColorOf(L.node->name);
        float r, g, b;
        Rgb01(base, r, g, b);
        ImU32 cTL = LitRgb(r, g, b, dark ? 0.22f : 0.16f);
        ImU32 cBR = LitRgb(r, g, b, dark ? -0.15f : -0.11f);
        float mr = (r + r) * 0.5f, mg = (g + g) * 0.5f, mb = (b + b) * 0.5f;
        float a0 = dark ? 0.22f : 0.16f, a1 = dark ? -0.15f : -0.11f;
        float mid = (a0 + a1) * 0.5f;
        ImU32 cMid = LitRgb(r, g, b, mid);
        (void)mr; (void)mg; (void)mb;

        ImDrawVert v[4];
        ImVec2 uv = ImGui::GetDrawListSharedData()->TexUvWhitePixel;
        v[0].pos = ImVec2(s_panelPos.x + x0, s_panelPos.y + y0); v[0].uv = uv; v[0].col = cTL;
        v[1].pos = ImVec2(s_panelPos.x + x1, s_panelPos.y + y0); v[1].uv = uv; v[1].col = cMid;
        v[2].pos = ImVec2(s_panelPos.x + x1, s_panelPos.y + y1); v[2].uv = uv; v[2].col = cBR;
        v[3].pos = ImVec2(s_panelPos.x + x0, s_panelPos.y + y1); v[3].uv = uv; v[3].col = cMid;
        ImDrawIdx bidx = (ImDrawIdx)s_vtx.size();
        s_vtx.push_back(v[0]); s_vtx.push_back(v[1]);
        s_vtx.push_back(v[2]); s_vtx.push_back(v[3]);
        s_idx.push_back(bidx + 0); s_idx.push_back(bidx + 1); s_idx.push_back(bidx + 2);
        s_idx.push_back(bidx + 0); s_idx.push_back(bidx + 2); s_idx.push_back(bidx + 3);

        if (sw >= 34.0f && sh >= 18.0f) s_labelLeaves.push_back(&L);
        if (sw >= 10.0f && sh >= 10.0f) s_rimLeaves.push_back(&L);
    }

    // 1px 边缘浮雕（片元着色器的 +0.06 上/左、-0.05 下/右）；
    // 只对 ≥10px 的块做（小块的整体明度已由渐变近似），避免顶点数翻倍
    {
        auto addQuad = [&](float x0, float y0, float x1, float y1, ImU32 col) {
            if (x1 <= x0 || y1 <= y0) return;
            ImDrawVert v[4];
            ImVec2 uv = ImGui::GetDrawListSharedData()->TexUvWhitePixel;
            v[0].pos = ImVec2(s_panelPos.x + x0, s_panelPos.y + y0); v[0].uv = uv; v[0].col = col;
            v[1].pos = ImVec2(s_panelPos.x + x1, s_panelPos.y + y0); v[1].uv = uv; v[1].col = col;
            v[2].pos = ImVec2(s_panelPos.x + x1, s_panelPos.y + y1); v[2].uv = uv; v[2].col = col;
            v[3].pos = ImVec2(s_panelPos.x + x0, s_panelPos.y + y1); v[3].uv = uv; v[3].col = col;
            ImDrawIdx b2 = (ImDrawIdx)s_vtx.size();
            s_vtx.push_back(v[0]); s_vtx.push_back(v[1]);
            s_vtx.push_back(v[2]); s_vtx.push_back(v[3]);
            s_idx.push_back(b2 + 0); s_idx.push_back(b2 + 1); s_idx.push_back(b2 + 2);
            s_idx.push_back(b2 + 0); s_idx.push_back(b2 + 2); s_idx.push_back(b2 + 3);
        };
        for (const Leaf* lp : s_rimLeaves) {
            const Leaf& L = *lp;
            ImU32 base = L.node->isDir ? ColTmDir() : ExtColorOf(L.node->name);
            float r, g, b;
            Rgb01(base, r, g, b);
            ImU32 hi = LitRgb(r, g, b, 0.06f);
            ImU32 lo = LitRgb(r, g, b, -0.05f);
            float x0 = VX(L.x), y0 = VX(L.y);
            float x1 = VX(L.x + L.w), y1 = VX(L.y + L.h);
            addQuad(x0, y0, x1, y0 + 1.0f, hi);            // 上
            addQuad(x0, y0, x0 + 1.0f, y1, hi);            // 左
            addQuad(x0, y1 - 1.0f, x1, y1, lo);            // 下
            addQuad(x1 - 1.0f, y0, x1, y1, lo);            // 右
        }
    }

    // 目录外框：只画【最外层】深度（HTML curDepth = min(liveDirs.depth)）
    if (!s_dirs.empty()) {
        int minD = INT_MAX;
        for (const auto& d : s_dirs) minD = std::min(minD, d.depth);
        for (const auto& d : s_dirs) if (d.depth == minD) s_frameDirs.push_back(&d);
    }
}

// ------------------------------------------------------------ 布局入口 ----
static size_t ExtMapHash() {
    size_t h = 1469598103934665603ull;
    for (const auto& g2 : g_extMap) {
        h = (h ^ (size_t)g2.color) * 1099511628211ull;
        for (const auto& e : g2.exts)
            for (wchar_t c : e) h = (h ^ (size_t)c) * 1099511628211ull;
    }
    h = (h ^ (size_t)g_extFallback) * 1099511628211ull;
    return h;
}

static void LayoutAll(float w, float h) {
    s_leaves.clear(); s_dirs.clear();
    if (!g_current || w < 4.0f || h < 4.0f) { BakeAll(); return; }
    if (g_current->children.empty()) { BakeAll(); return; }

    unsigned long long t0 = GetTickCount64();
    double W = (double)w * (double)LAYOUT_SCALE;
    double H = (double)h * (double)LAYOUT_SCALE;
    std::unordered_map<ScanNode*, double> memo;
    memo.reserve(4096);
    CollectLeaves(g_current->children, 0.0, 0.0, W, H, 0, 999.0, g_st, memo, s_leaves, s_dirs);
    s_layoutMs = (double)(GetTickCount64() - t0);
    BakeAll();

    // 诊断：与 web/index.html 的 diag{what:"bake"} 对标（同一目录应得到同样的
    // 叶子数 / 前 5 大块 px² 比例），并给出「铺满率」用于核对是否画全
    {
        std::vector<const Leaf*> top;
        top.reserve(s_leaves.size());
        double cover = 0.0;
        int tiny = 0;
        for (const Leaf& L : s_leaves) {
            top.push_back(&L);
            cover += L.w * L.h;
            if (L.w < 1.0 || L.h < 1.0) tiny++;
        }
        std::sort(top.begin(), top.end(),
                  [](const Leaf* a, const Leaf* b) { return a->w * a->h > b->w * b->h; });
        wchar_t cov[32];
        swprintf(cov, 32, L"%.4f", cover / (W * H));
        // 叶子包围盒 + 烘焙顶点包围盒（核对「画出来的是不是铺满整块画布」）
        double lx0 = 1e18, ly0 = 1e18, lx1 = -1e18, ly1 = -1e18;
        for (const Leaf& L : s_leaves) {
            lx0 = std::min(lx0, L.x); ly0 = std::min(ly0, L.y);
            lx1 = std::max(lx1, L.x + L.w); ly1 = std::max(ly1, L.y + L.h);
        }
        float vx0 = 1e18f, vy0 = 1e18f, vx1 = -1e18f, vy1 = -1e18f;
        for (const ImDrawVert& v : s_vtx) {
            vx0 = std::min(vx0, v.pos.x); vy0 = std::min(vy0, v.pos.y);
            vx1 = std::max(vx1, v.pos.x); vy1 = std::max(vy1, v.pos.y);
        }
        std::wstring line = L"TMBAKE cur=" + (g_current ? g_current->name : L"") +
                            L" allLeaves=" + std::to_wstring(s_leaves.size()) +
                            L" allDirs=" + std::to_wstring(s_dirs.size()) + L" W=" +
                            std::to_wstring((long long)W) + L" H=" + std::to_wstring((long long)H) +
                            L" leafBBox=" + std::to_wstring((long long)lx0) + L"," +
                            std::to_wstring((long long)ly0) + L"-" +
                            std::to_wstring((long long)lx1) + L"," +
                            std::to_wstring((long long)ly1) + L" vtxBBox=" +
                            std::to_wstring((long long)vx0) + L"," + std::to_wstring((long long)vy0) +
                            L"-" + std::to_wstring((long long)vx1) + L"," +
                            std::to_wstring((long long)vy1) + L" panel=" +
                            std::to_wstring((long long)s_panelPos.x) + L"," +
                            std::to_wstring((long long)s_panelPos.y) + L" " +
                            std::to_wstring((long long)s_panelW) + L"x" +
                            std::to_wstring((long long)s_panelH) + L" ms=" +
                            std::to_wstring((long long)s_layoutMs) + L" cover=" + cov +
                            L" tiny=" + std::to_wstring(tiny) + L" top=";
        for (size_t i = 0; i < 5 && i < top.size(); i++) {
            if (i) line += L" | ";
            line += top[i]->node->name + L"=" + std::to_wstring(top[i]->node->size) + L"B/" +
                    std::to_wstring((long long)(top[i]->w * top[i]->h)) + L"px2";
        }
        LogLine(line);
    }
}

// ------------------------------------------------------- 命中 / 并集框 ----
static Leaf* HitTest(float px, float py) {
    float X = floorf(px), Y = floorf(py);
    for (size_t i = s_leaves.size(); i-- > 0;) {
        float x0, y0, x1, y1;
        TmRect(s_leaves[i], x0, y0, x1, y1);
        if (x1 - x0 < 1.0f || y1 - y0 < 1.0f) continue;
        if (X >= x0 && X < x1 && Y >= y0 && Y < y1) return &s_leaves[i];
    }
    return nullptr;
}

static bool IsDescendantOf(const ScanNode* node, const ScanNode* anc) {
    for (const ScanNode* p = node->parent; p; p = p->parent) if (p == anc) return true;
    return false;
}

static bool UnionRectOf(const ScanNode* id, float& x0, float& y0, float& x1, float& y1) {
    bool hit = false;
    for (const Leaf& r : s_leaves) {
        if (r.node != id && !IsDescendantOf(r.node, id)) continue;
        float a, b, c, d;
        TmRect(r, a, b, c, d);
        if (!hit) { x0 = a; y0 = b; x1 = c; y1 = d; hit = true; }
        else { x0 = std::min(x0, a); y0 = std::min(y0, b); x1 = std::max(x1, c); y1 = std::max(y1, d); }
    }
    return hit;
}

static void ResetWheelFocus(ScanNode* currentId) {
    s_wheelStack.clear();
    s_wheelId = nullptr;
    s_wheelHas = false;
    s_wheelBaseHover = currentId;
}

static bool WithinWheelWindow() {
    return (ImGui::GetTime() * 1000.0 - s_lastWheelTick) < WHEEL_MS;
}

// ---------------------------------------------------------- 信息面板 ----
static void DrawInfoLabel(ImDrawList* dl, float rx0, float ry0, float rx1, float ry1,
                          ScanNode* node) {
    if (!node) return;
    double total = (g_current && g_current->size) ? (double)g_current->size : 1.0;
    char pct[32];
    snprintf(pct, sizeof(pct), "%.1f", (double)node->size / total * 100.0);

    std::wstring kind = L"文件";
    if (node->isDir) {
        int sub = 0;
        for (ScanNode* c : node->children) if (c->isDir) sub++;
        kind = L"目录 · " + std::to_wstring(sub) + L" 项";
    }
    std::string l0 = Utf8(FullPathOf(node));
    std::string l1 = Utf8(node->name);
    std::string l2 = FmtSize(node->size) + " \xc2\xb7 " + pct + "%";
    std::string l3 = Utf8(kind);

    const float padX = 8.0f, padY = 5.0f, lh = 18.0f;
    float maxTW = std::min(s_panelW - 16.0f, std::max(160.0f, s_panelW * 0.7f));
    ImFont* f = g_fontUi ? g_fontUi : ImGui::GetFont();
    float fs = 13.0f;
    std::string lines[4] = { l0, l1, l2, l3 };
    for (auto& s : lines) s = Ellipsize(s.c_str(), maxTW - padX * 2, f, fs);

    float tw = 0;
    for (auto& s : lines) {
        ImVec2 sz = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, s.c_str());
        tw = std::max(tw, sz.x);
    }
    float bw = tw + padX * 2, bh = 4 * lh + padY * 2;

    // 贴框外侧：右上 → 左上 → 右下 → 左下 → 框内左上（都要留在画布内）
    float bx = rx1 + 6, by = ry0 - bh - 6;
    if (bx + bw > s_panelW - 2) bx = rx0 - bw - 6;
    if (bx < 2) bx = std::min(rx0 + 2, std::max(2.0f, s_panelW - bw - 2));
    if (by < 2) by = std::min(ry1 + 6, std::max(2.0f, s_panelH - bh - 2));
    if (by + bh > s_panelH - 2) by = std::max(2.0f, s_panelH - bh - 2);

    ImVec2 p0(s_panelPos.x + bx, s_panelPos.y + by);
    ImVec2 p1(p0.x + bw, p0.y + bh);
    dl->AddRectFilled(p0, p1, IM_COL32(20, 23, 28, 224));
    dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 89));
    const ImU32 cols[4] = {
        IM_COL32(255, 255, 255, 158),   // 路径 .62
        IM_COL32(255, 255, 255, 255),   // 名称
        IM_COL32(255, 255, 255, 199),   // 大小·占比 .78
        IM_COL32(255, 255, 255, 140),   // 文件/目录 .55
    };
    for (int i = 0; i < 4; i++)
        dl->AddText(f, fs, ImVec2(p0.x + padX, p0.y + padY + i * lh), cols[i], lines[i].c_str());
}

// ================================================================ 绘制 =====
void TreemapInvalidate() { s_keyStamp = -1; }

void TreemapOnEnter() {
    g_animActive = true;
    g_animT0 = ImGui::GetTime() * 1000.0;
}

bool TreemapWheelActive() { return s_wheelHas; }

// 导航/重扫时作废滚轮框选（对应 HTML enter() 里的 wheelStack 清空）
void TreemapClearWheel() { ResetWheelFocus(nullptr); }
ScanNode* TreemapWheelNode() { return s_wheelHas ? s_wheelId : nullptr; }

void DrawTreemapPanel(const ImVec2& size) {
    s_panelPos = ImGui::GetCursorScreenPos();
    s_panelW = size.x;
    s_panelH = size.y;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // 底色（HTML: .right = GL clearColor）
    dl->AddRectFilled(s_panelPos, ImVec2(s_panelPos.x + size.x, s_panelPos.y + size.y), ColTmGround());
    if (size.x < 8 || size.y < 8) return;

    // ---- 布局缓存：当前目录 / 画布尺寸 / 主题 / 设置 / 配色 变化才重算 ----
    size_t extHash = ExtMapHash();
    bool need = (s_keyNode != g_current) || (s_keyW != size.x) || (s_keyH != size.y) ||
                (s_keyStamp != g_layoutStamp) || (s_keyTheme != g_st.theme) ||
                (s_keyExtHash != extHash);
    if (need) {
        // 烘焙顶点里含面板绝对坐标 → 面板移动也要重烘焙
        LayoutAll(size.x, size.y);
        s_keyNode = g_current; s_keyW = size.x; s_keyH = size.y;
        s_keyStamp = g_layoutStamp; s_keyTheme = g_st.theme; s_keyExtHash = extHash;
        // HTML：每次烘焙都把 tmHoverId 清空（bakeView 里 tmHoverId = null），
        // 否则布局变了光标下的块也变了，焦点框会停在旧块上
        s_hoverNode = nullptr;
        s_hoverLeaf = nullptr;
        s_lastMouse = ImVec2(-1e9f, -1e9f);
        // 滚轮框选的并集框是按【旧布局】算出来的坐标：布局一变就必须作废，
        // 否则会留下一个位置/大小都不对的暗框（HTML 在 enter() 里清空滚轮会话）
        ResetWheelFocus(nullptr);
    }

    // ---- 提交色块顶点 ----
    // 【关键】ImGui 的 ImDrawIdx 是 16 位（1.92/1.93 没有 32 位索引开关），
    // 一次 PrimReserve 提交 66 万顶点会触发 ImGui 的 VtxOffset 分段，而手工写入的
    // 绝对索引会被后端再加上 BaseVertex → 整片色块错位（表现为「图只画了一小块」）。
    // 正确做法：按 ≤16000 个四边形（64000 顶点）分块提交，索引以每块
    // PrimReserve 之后的 _VtxCurrentIdx 为基准 —— 这样无论 ImGui 是否切换
    // VtxOffset，索引都是对的。
    float anim = AnimT();
    const size_t MAXQ = 16000;
    size_t totalQ = s_idx.size() / 6;
    for (size_t q = 0; q < totalQ; q += MAXQ) {
        size_t n = std::min(MAXQ, totalQ - q);
        dl->PrimReserve((int)(n * 6), (int)(n * 4));
        ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
        ImDrawVert* vw = dl->_VtxWritePtr;
        ImDrawIdx* iw = dl->_IdxWritePtr;
        if (anim >= 0.999f) {
            memcpy(vw, s_vtx.data() + q * 4, n * 4 * sizeof(ImDrawVert));
        } else {
            float sc = 0.965f + 0.035f * anim;
            ImVec2 c(s_panelPos.x + size.x * 0.5f, s_panelPos.y + size.y * 0.4f);
            int alphaMul = (int)(anim * 255.0f + 0.5f);
            for (size_t i = 0; i < n * 4; i++) {
                ImDrawVert v = s_vtx[q * 4 + i];
                v.pos.x = c.x + (v.pos.x - c.x) * sc;
                v.pos.y = c.y + (v.pos.y - c.y) * sc;
                int a = (int)((v.col >> IM_COL32_A_SHIFT) & 255);
                a = a * alphaMul / 255;
                v.col = (v.col & ~IM_COL32_A_MASK) | ((ImU32)a << IM_COL32_A_SHIFT);
                vw[i] = v;
            }
        }
        for (size_t k = 0; k < n; k++) {
            ImDrawIdx b = (ImDrawIdx)(base + (ImDrawIdx)(k * 4));
            iw[k * 6 + 0] = b;
            iw[k * 6 + 1] = (ImDrawIdx)(b + 1);
            iw[k * 6 + 2] = (ImDrawIdx)(b + 2);
            iw[k * 6 + 3] = b;
            iw[k * 6 + 4] = (ImDrawIdx)(b + 2);
            iw[k * 6 + 5] = (ImDrawIdx)(b + 3);
        }
        dl->_VtxWritePtr += n * 4;
        dl->_IdxWritePtr += n * 6;
        dl->_VtxCurrentIdx += (ImDrawIdx)(n * 4);
    }

    // 面板矩形（屏幕坐标）
    ImVec2 panelMin = s_panelPos;
    ImVec2 panelMax(s_panelPos.x + size.x, s_panelPos.y + size.y);

    // ---- 目录外框（最外层，1 物理像素）----
    const float lw = HAIR;   // HTML: lw = 1/tmDpr（物理 1px）
    ImU32 frameCol = ColTmFrame();
    for (const DirRect* dp : s_frameDirs) {
        const DirRect& d = *dp;
        // 吸附到物理像素：否则 1px 细线落半像素上被 GPU 覆盖一半 → 看着像没画
        float ax = SnapPx(panelMin.x + VX(d.x)), ay = SnapPx(panelMin.y + VX(d.y));
        float bx = SnapPx(panelMin.x + VX(d.x + d.w)), by = SnapPx(panelMin.y + VX(d.y + d.h));
        if (bx - ax < 8 || by - ay < 8) continue;
        ImVec2 a(ax, ay), b(bx, by);
        dl->AddRectFilled(a, ImVec2(b.x, a.y + lw), frameCol);
        dl->AddRectFilled(ImVec2(a.x, b.y - lw), b, frameCol);
        dl->AddRectFilled(a, ImVec2(a.x + lw, b.y), frameCol);
        dl->AddRectFilled(ImVec2(b.x - lw, a.y), b, frameCol);
    }

    // ---- 文字（名称 + 大小；clip 到块内 + 省略号）----
    {
        const float pad = 3.0f;
        for (const Leaf* lp : s_labelLeaves) {
            const Leaf& r = *lp;
            ScanNode* n = r.node;
            float x0 = panelMin.x + VX(r.x), y0 = panelMin.y + VX(r.y);
            float w = VX(r.x + r.w) - VX(r.x), h = VX(r.y + r.h) - VX(r.y);
            if (w < 34 || h < 18) continue;
            dl->PushClipRect(ImVec2(x0, y0), ImVec2(x0 + w, y0 + h), true);
            float availW = w - pad * 2;
            if (availW > 14 && h > 15.0f) {
                ImFont* f = (w >= 110.0f && g_fontSm) ? g_fontSm : (g_fontXs ? g_fontXs : ImGui::GetFont());
                float fs = (w >= 110.0f) ? 12.0f : 11.0f;
                std::string nm = Ellipsize(Utf8(n->name).c_str(), availW, f, fs);
                if (!nm.empty())
                    dl->AddText(f, fs, ImVec2(x0 + pad, y0 + pad), IM_COL32(255, 255, 255, 255),
                                nm.c_str());
                if (h >= 34.0f && w >= 110.0f) {
                    std::string sz = Ellipsize(FmtSize(n->size).c_str(), availW, f, fs);
                    if (!sz.empty())
                        dl->AddText(f, fs, ImVec2(x0 + pad, y0 + pad + 16.0f),
                                    IM_COL32(255, 255, 255, 235), sz.c_str());
                }
            }
            dl->PopClipRect();
        }
    }
    (void)anim;

    // ---- 命中测试（鼠标未动则复用上次结果）----
    ImVec2 mouse = ImGui::GetIO().MousePos;
    // 有弹出层（下拉菜单 / 设置模态 / 输入弹窗）时，树图不接收悬停·点击·滚轮。
    // 否则点下拉菜单里的项会「穿透」到下面的树图上，顺带把选中/焦点也一起改了
    // （用户报的穿透问题）。ImGui 的窗口命中会被弹层挡住，但这里的 inside 是
    // 自己按矩形算的，必须显式挡住。
    const bool uiBlocked = g_showSettings || g_renameOpen || g_newFolderOpen ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    bool inside = !uiBlocked &&
                  mouse.x >= panelMin.x && mouse.x < panelMax.x &&
                  mouse.y >= panelMin.y && mouse.y < panelMax.y;
    if (mouse.x != s_lastMouse.x || mouse.y != s_lastMouse.y || !s_mouseValid) {
        s_lastMouse = mouse;
        s_mouseValid = true;
        Leaf* hit = inside ? HitTest(mouse.x - panelMin.x, mouse.y - panelMin.y) : nullptr;
        ScanNode* hitNode = hit ? hit->node : nullptr;
        if (hitNode != s_hoverNode) {
            // 鼠标移到别的块 → 结束上一次上滚会话，焦点跟着鼠标走
            if (s_wheelHas && hitNode != s_wheelBaseHover) ResetWheelFocus(hitNode);
            s_hoverNode = hitNode;
            s_hoverLeaf = hit;
        } else {
            s_hoverLeaf = hit;
        }
    } else if (!inside) {
        if (s_hoverNode) {
            if (s_wheelHas) ResetWheelFocus(nullptr);
            s_hoverNode = nullptr;
            s_hoverLeaf = nullptr;
        }
    }

    // ---- 滚轮：逐级父级框选 ----
    if (inside && ImGui::GetIO().MouseWheel != 0.0f) {
        s_lastWheelTick = ImGui::GetTime() * 1000.0;
        bool up = ImGui::GetIO().MouseWheel > 0.0f;
        if (up) {
            if (s_hoverNode && s_wheelBaseHover != s_hoverNode) {
                s_wheelStack.clear(); s_wheelId = nullptr; s_wheelHas = false;
                s_wheelBaseHover = s_hoverNode;
            }
            if (!s_hoverNode && s_wheelBaseHover) {
                s_wheelStack.clear(); s_wheelId = nullptr; s_wheelHas = false;
                s_wheelBaseHover = nullptr;
            }
            ScanNode* start = s_wheelId ? s_wheelId
                             : (s_hoverNode ? s_hoverNode : g_selNode);
            if (start) {
                ScanNode* par = start->parent;
                float x0, y0, x1, y1;
                if (par && UnionRectOf(par, x0, y0, x1, y1)) {
                    s_wheelStack.push_back({ s_wheelId, s_wheelHas, s_wheelX0, s_wheelY0, s_wheelX1, s_wheelY1 });
                    s_wheelId = par; s_wheelHas = true;
                    s_wheelX0 = x0; s_wheelY0 = y0; s_wheelX1 = x1; s_wheelY1 = y1;
                }
            }
        } else {
            if (!s_wheelStack.empty()) {
                WheelEnt e = s_wheelStack.back();
                s_wheelStack.pop_back();
                s_wheelId = e.id; s_wheelHas = e.has;
                s_wheelX0 = e.x0; s_wheelY0 = e.y0; s_wheelX1 = e.x1; s_wheelY1 = e.y1;
                if (!s_wheelId) s_wheelBaseHover = s_hoverNode;
            } else {
                s_wheelId = nullptr; s_wheelHas = false;
                s_wheelBaseHover = s_hoverNode;
            }
        }
    }

    // ---- 焦点框：滚轮框选 > 悬停 > 选中（同一时刻只画一个）----
    if (!s_wheelHas) {
        ScanNode* id = s_hoverNode ? s_hoverNode : g_selNode;
        if (id) {
            float x0 = -1, y0 = -1, x1 = -1, y1 = -1;
            Leaf* lf = nullptr;
            for (Leaf& L : s_leaves) if (L.node == id) { lf = &L; break; }
            if (lf) TmRect(*lf, x0, y0, x1, y1);
            else UnionRectOf(id, x0, y0, x1, y1);
            if (x0 >= 0 && x1 - x0 >= 1.0f && y1 - y0 >= 1.0f) {
                ImVec2 a(panelMin.x + x0, panelMin.y + y0), b(panelMin.x + x1, panelMin.y + y1);
                ImU32 stroke = s_hoverNode ? IM_COL32(255, 255, 255, 217) : IM_COL32(255, 255, 255, 242);
                dl->AddRectFilled(a, ImVec2(b.x, a.y + lw), stroke);
                dl->AddRectFilled(ImVec2(a.x, b.y - lw), b, stroke);
                dl->AddRectFilled(a, ImVec2(a.x + lw, b.y), stroke);
                dl->AddRectFilled(ImVec2(b.x - lw, a.y), b, stroke);
                DrawInfoLabel(dl, x0, y0, x1, y1, id);
            }
        }
    } else {
        // 框外压暗 + 白框 + 信息标签（框与信息一体）
        ImVec2 a(panelMin.x + (float)s_wheelX0, panelMin.y + (float)s_wheelY0);
        ImVec2 b(panelMin.x + (float)s_wheelX1, panelMin.y + (float)s_wheelY1);
        ImU32 dim = IM_COL32(0, 0, 0, 89);
        dl->AddRectFilled(panelMin, ImVec2(panelMax.x, a.y), dim);
        dl->AddRectFilled(ImVec2(panelMin.x, b.y), panelMax, dim);
        dl->AddRectFilled(ImVec2(panelMin.x, a.y), ImVec2(a.x, b.y), dim);
        dl->AddRectFilled(ImVec2(b.x, a.y), ImVec2(panelMax.x, b.y), dim);
        dl->AddRectFilled(a, ImVec2(b.x, a.y + lw), IM_COL32(255, 255, 255, 255));
        dl->AddRectFilled(ImVec2(a.x, b.y - lw), b, IM_COL32(255, 255, 255, 255));
        dl->AddRectFilled(a, ImVec2(a.x + lw, b.y), IM_COL32(255, 255, 255, 255));
        dl->AddRectFilled(ImVec2(b.x - lw, a.y), b, IM_COL32(255, 255, 255, 255));
        DrawInfoLabel(dl, (float)s_wheelX0, (float)s_wheelY0, (float)s_wheelX1, (float)s_wheelY1, s_wheelId);
    }

    // ---- 点击：只改焦点 + 列表联动定位（HTML 不改多选集合）----
    if (inside && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ScanNode* n = s_wheelHas ? s_wheelId : s_hoverNode;
        if (DbgAutoClick())   // 自检：验证下拉菜单弹出时不再穿透到树图
            LogLine(std::wstring(L"TMCLICK node=") + (n ? n->name : L"(null)"));
        if (n && !WithinWheelWindow()) {
            g_selNode = n;
            LocateInList(n);
        }
    }
    if (inside && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        ScanNode* n = s_wheelHas ? s_wheelId : s_hoverNode;
        if (n && !WithinWheelWindow()) {
            if (n->isDir) Enter(n);
            else ShellOpenPath(FullPathOf(n));
        }
    }
}
