// 树图视图 v3：递归下钻 + 扁平现代化风格
//   - 递归：文件夹按设置（层级/最小尺寸/矩形数）展开为其子项
//   - 单击=选中，双击=进入（目录）或打开（文件），右键=浮出菜单
//   - 滚轮：上滚=选择父级，下滚=回（上滚前的选中）
//   - 悬停：高亮 + 悬浮信息面板（名称/大小/占比）
//   - 无矩形边框，1px 间隙，深色扁平 UI
//   - 色块渐变：Direct2D 线性渐变刷（GPU 逐像素平滑插值）；D2D 不可用时回退 GDI 分段

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <functional>
#include <map>
#include <vector>

#include "scanner.h"
#include "theme.h"
#include "treemap.h"
#include "utils.h"

// ================= 纯布局算法（可独立测试） =================

namespace {

// 面积权重：log2(size+2)。对数刻度压缩量级差异（KB 与 GB 权重仅差 ~3 倍），
// 避免大文件吞掉整个视图、小文件不可见；0 字节项权重 1 仍可见。
double AreaOf(const ScanNode* n) { return log2((double)n->size + 2.0); }

// ===== 面积算法（映射 g + 顺序，与 Web 版一致，设置来自 config.json） =====
// mapKind: 0=x 1=log₂ 2=log₂² 3=√x 4=x^α；ordKind: 0=先和后 g(Σx) 1=先 g 后 Σg(x)
static double G(double v, const AppSettings& st) {
    v = std::max(v, 1.0);
    switch (st.mapKind) {
        case 1: return log2(v + 2.0);
        case 2: { double l = log2(v + 2.0); return l * l; }
        case 3: return sqrt(v);
        case 4: { double a = st.powAlpha; if (!(a > 0.01)) a = 0.5; return pow(v, a); }
        default: return v;
    }
}
// Gsum：目录 = Σ g(每个后代文件)；兜底不小于自身的 g
static double Gsum(const ScanNode* n, const AppSettings& st,
                   std::unordered_map<const ScanNode*, double>& memo) {
    auto it = memo.find(n);
    if (it != memo.end()) return it->second;
    double v;
    if (n->children.empty()) {
        v = G((double)n->size, st);
    } else {
        v = 0;
        for (auto* c : n->children) v += Gsum(c, st, memo);
        v = std::max(v, G(std::max((double)n->size, 1.0), st));
    }
    memo[n] = v;
    return v;
}
// 布局权重（ComputeTreemap 的 A 函数）；与 Web 版 wOf 完全一致：
//   ordKind 0=sumG: g(Σx)  1=Gsum: Σg(leaf)  2=g(Σg(leaf))
static double ItemWeight(const ScanNode* n, const AppSettings& st,
                         std::unordered_map<const ScanNode*, double>& memo) {
    double s = std::max((double)n->size, 1.0);
    if (st.mapKind == 0) return s;                    // id：三种顺序同结果
    if (st.ordKind == 0) return G(s, st);             // sumG：g(Σx)
    double gs = Gsum(n, st, memo);
    if (st.ordKind == 1) return gs;                   // Gsum：Σg(leaf)
    return G(gs, st);                                 // g(Σg(leaf))
}

double WorstAspect(const std::vector<ScanNode*>& row, ScanNode* extra, double sum, double w,
                   double h, double pxPerUnit,
                   const std::function<double(const ScanNode*)>& A) {
    if (row.empty() && !extra) return 1e18;
    double area = sum * pxPerUnit;
    double longSide = (w >= h) ? w : h;
    double shortSide = area / longSide;
    if (shortSide <= 0) return 1e18;
    double worst = 0;
    auto consider = [&](double size) {
        double dim = size * pxPerUnit / shortSide;
        double a = dim / shortSide;
        if (a < 1.0) a = 1.0 / a;
        if (a > worst) worst = a;
    };
    for (auto* n : row) consider(A(n));
    if (extra) consider(A(extra));
    return worst;
}

// 竖直方向的行（宽<高）：块高下限（名称 + 大小两行文字所需像素）
double MinRowDim(const AppSettings* st) {
    if (!st || st->rowExtraPx <= 0) return 1.0;
    return 24.0 + st->rowExtraPx;
}

// 按「行」分组：同一行 = 顶边落在同一个像素高度上的矩形（行内矩形互不重叠、并排铺开）。
// 注意：不能用「顶边 = 上一项底边」，因为竖直方向的行（宽<高）会跨多行。
static void GroupRows(const std::vector<TreemapItem>& out, std::vector<std::vector<size_t>>& rows) {
    std::map<int, std::vector<size_t>> byTop;
    for (size_t i = 0; i < out.size(); i++) byTop[out[i].rc.top].push_back(i);
    for (auto& kv : byTop) rows.push_back(kv.second);
}

// 一组同顶边矩形的落点：全部底边相同 = 真正的「行」（横条并排，可安全交换像素）；
// 底边不同 = 竖直堆叠（竖条），像素转移会把整段顶边推下去，禁止处理。
static bool RowIsBarRun(const std::vector<TreemapItem>& out, const std::vector<size_t>& row,
                        int* bottom) {
    if (row.size() < 2) return false;
    int b0 = out[row[0]].rc.bottom;
    for (size_t k : row)
        if (out[k].rc.bottom != b0) return false;
    *bottom = b0;
    return true;
}

// 抬升最矮矩形的高度：让出像素的矩形收缩（保持顶边，行内顺序不受影响），
// 被抬升的矩形只有一条边在动（上边下移 —— 该边紧邻的就是原行上方的矩形，
// 且本行内无人与它共享该边；等价于上方矩形「变高」），因此不会跨越或覆盖别的块。
// 只处理横条行（并排铺开）；竖直堆叠一律不改（改了会推走整段顶边、造成重叠）。
void EnforceMinHeight(std::vector<TreemapItem>& out, int width, int height, double minDim) {
    if (minDim <= 1.5) return;
    std::vector<std::vector<size_t>> rows;
    GroupRows(out, rows);

    for (auto& row : rows) {
        int rowBottom = 0;
        if (!RowIsBarRun(out, row, &rowBottom)) continue;
        int rowTop = out[row[0]].rc.top;
        int limit = rowTop + (int)minDim;
        if (limit > rowBottom) continue;  // 行本身太矮，无论如何都满足不了
        // 自矮到高抬升；让出者始终是「当前最高」的矩形（保持行内高度秩序）
        std::vector<size_t> order = row;
        std::stable_sort(order.begin(), order.end(), [&](size_t a2, size_t b2) {
            int ha = out[a2].rc.bottom - out[a2].rc.top;
            int hb = out[b2].rc.bottom - out[b2].rc.top;
            if (ha != hb) return ha < hb;
            return a2 < b2;
        });
        for (size_t oi = 0; oi + 1 < order.size(); oi++) {
            size_t low = order[oi];
            int need = (int)minDim - (out[low].rc.bottom - out[low].rc.top);
            if (need <= 0) continue;
            for (size_t oj = order.size(); oj-- > oi + 1;) {
                size_t donor = order[oj];
                int dh = out[donor].rc.bottom - out[donor].rc.top;
                int give = dh - limit;
                if (give <= 0) continue;
                if (give > need) give = need;
                out[donor].rc.bottom -= give;   // 收缩：顶边不动
                out[low].rc.top += give;        // 抬升：只有上边动（下边与行底对齐不动）
                need -= give;
                if (need <= 0) break;
            }
        }
    }
}

}  // namespace

void ComputeTreemap(const std::vector<ScanNode*>& items, int width, int height,
                    size_t maxItems, std::vector<TreemapItem>& out,
                    const std::function<double(const ScanNode*)>& wf, int layoutId) {
    out.clear();
    if (width < 4 || height < 4) return;
    auto A = [&](const ScanNode* n) { return wf ? wf(n) : AreaOf(n); };

    std::vector<ScanNode*> v;
    v.reserve(items.size());
    for (auto* n : items)
        if (n && n->size > 0) v.push_back(n);
    if (v.empty()) return;
    std::stable_sort(v.begin(), v.end(),
                     [](const ScanNode* a, const ScanNode* b) {
                         if (a->isDir != b->isDir) return a->isDir > b->isDir;  // 目录优先
                         return a->size > b->size;                              // 同级 size 降序
                     });
    if (v.size() > maxItems && maxItems > 0) v.resize(maxItems);   // 0 = 不限

    double total = 0;
    for (auto* n : v) total += A(n);
    if (total <= 0) return;

    const double pxPerUnit = (double)width * height / total;

    double x = 0, y = 0, w = (double)width, h = (double)height;
    std::vector<ScanNode*> row;
    double rowSum = 0;

    auto layoutRow = [&](bool extend = false) {
        if (row.empty()) return;
        size_t rowStart = out.size();  // 本行起始索引（并入时只并入本行矩形）
        double rowArea = rowSum * pxPerUnit;
        bool horiz = (w >= h);
        double rowThick = rowArea / (horiz ? w : h);
        if (rowThick < 1) rowThick = 1;  // 行厚保底 1px：避免 <1px 行行间叠加
        size_t cnt = row.size();
        // largest-remainder：每项取 floor(权重尺寸)，剩余像素按小数降序 +1px，
        // 行内精确填满（覆盖 100%），每项浮动 ≤1px（取对数前，大小顺序基本不被破坏）
        std::vector<double> dims(cnt);
        std::vector<int> fl(cnt);
        std::vector<int> idxs(cnt);
        double sumFl = 0;
        int avail = horiz ? ((int)(x + w) - (int)x) : ((int)(y + h) - (int)y);
        for (size_t i = 0; i < cnt; i++) {
            dims[i] = A(row[i]) * pxPerUnit / rowThick;
            fl[i] = (int)dims[i];
            if (fl[i] < 1) fl[i] = 1;
            sumFl += fl[i];
            idxs[i] = (int)i;
        }
        int remain = avail - (int)sumFl;
        std::stable_sort(idxs.begin(), idxs.end(), [&](int a, int b) {
            double fa = dims[a] - (int)dims[a];
            double fb = dims[b] - (int)dims[b];
            if (fa != fb) return fa > fb;
            return a < b;
        });
        for (size_t k = 0; k < cnt && remain != 0; k++) {
            int i = idxs[k];
            if (remain > 0 && fl[i] < avail) { fl[i]++; remain--; }
            else if (remain < 0 && fl[i] > 1) { fl[i]--; remain++; }
        }
        int cxI = (int)(horiz ? x : y);
        for (size_t i = 0; i < cnt; i++) {
            ScanNode* n = row[i];
            int iw = fl[i];
            int l, t, r, b;
            if (horiz) {
                l = cxI; t = (int)y; b = (int)(y + rowThick);
                r = l + iw;
                if (r > (int)(x + w)) r = (int)(x + w);
                cxI = r;
            } else {
                t = cxI; l = (int)x; r = (int)(x + rowThick);
                b = t + iw;
                if (b > (int)(y + h)) b = (int)(y + h);
                cxI = b;
            }
            if (l < 0) l = 0;
            if (t < 0) t = 0;
            if (r > width) r = width;
            if (b > height) b = height;
            if (r - l < 1) r = l + 1;  // 极小项强制至少 1px
            if (b - t < 1) b = t + 1;
            if (r > width) r = width;
            if (b > height) b = height;
            if (l < width && t < height && r > l && b > t) {
                out.push_back({ n, { l, t, r, b }, 0, layoutId });
            } else if (out.size() > rowStart && (r - l) <= 2 && (b - t) <= 2) {
                // 取整后出界的极小项：并入本行上一个矩形，但扩展 ≤ 其 1% 宽度
                TreemapItem& prev = out.back();
                int pw = prev.rc.right - prev.rc.left;
                int ph = prev.rc.bottom - prev.rc.top;
                if (horiz && pw > 0 && (r - prev.rc.right) <= std::max(1, pw / 100))
                    prev.rc.right = r;
                else if (!horiz && ph > 0 && (b - prev.rc.bottom) <= std::max(1, ph / 100))
                    prev.rc.bottom = b;
            }
        }
        // 底缘/右缘：最后一行延伸铺满（消除剩余空隙；面积浮动 ≤ 行厚 1%）
        if (extend) {
            if (horiz) {
                int bottom = (int)(y + h) - (int)(y + rowThick);
                if (bottom > 0 && bottom <= std::max(1, (int)rowThick / 100))
                    for (size_t k = rowStart; k < out.size(); k++) out[k].rc.bottom = height;
            } else {
                int right = (int)(x + w) - (int)(x + rowThick);
                if (right > 0 && right <= std::max(1, (int)rowThick / 100))
                    for (size_t k = rowStart; k < out.size(); k++) out[k].rc.right = width;
            }
        }
        if (horiz) { y += rowThick; h -= rowThick; }
        else { x += rowThick; w -= rowThick; }
        row.clear();
        rowSum = 0;
    };

    for (auto* n : v) {
        double newSum = rowSum + A(n);
        double ratio = WorstAspect(row, n, newSum, w, h, pxPerUnit, A);
        double cur = row.empty() ? 1e18 : WorstAspect(row, nullptr, rowSum, w, h, pxPerUnit, A);
        if (row.empty() || ratio <= cur) {
            row.push_back(n);
            rowSum = newSum;
        } else {
            layoutRow();
            row.push_back(n);
            rowSum = A(n);
        }
    }
    layoutRow(true);  // 最后一行延伸铺满，杜绝底缘/右缘空隙
}

void ComputeTreemapRecursive(const std::vector<ScanNode*>& items, int width, int height,
                             const AppSettings& st, std::vector<TreemapItem>& out) {
    out.clear();
    if (width < 4 || height < 4) return;

    const size_t kTopCap = 400;  // 顶层扇出上限

    std::vector<ScanNode*> top;
    for (auto* n : items)
        if (n && (n->size > 0 || n->isDir)) top.push_back(n);
    if (top.empty()) return;
    std::stable_sort(top.begin(), top.end(),
                     [](const ScanNode* a, const ScanNode* b) {
                         if (a->isDir != b->isDir) return a->isDir > b->isDir;  // 目录优先
                         return a->size > b->size;                              // 同级 size 降序
                     });
    if (top.size() > kTopCap) top.resize(kTopCap);

    // 面积权重：按设置里的映射 g 与顺序（默认 0=size，与原 log2 不同但语义一致：
    // 权重只影响面积分配比例，不改变递归结构）。Gsum 用 memo 缓存避免重复遍历。
    const ULONGLONG minBytes = (ULONGLONG)st.minSizeMB * 1024 * 1024;
    std::unordered_map<const ScanNode*, double> memo;
    memo.reserve(65536);
    auto A = [&](const ScanNode* n) { return ItemWeight(n, st, memo); };

    std::vector<TreemapItem> tops;
    ComputeTreemap(top, width, height, kTopCap, tops, A, 0);

    int nextLid = 1;  // 子布局 id 递增（同一布局实例 = 同一 id）
    std::function<void(ScanNode*, int, RECT, int)> rec = [&](ScanNode* node, int depth,
                                                             RECT rc, int lid) {
        bool expand = node->isDir && node->children.size() > 1 &&
                      (st.maxDepth == 0 || depth < st.maxDepth) &&
                      node->size >= minBytes && (st.maxRects == 0 || (int)out.size() < st.maxRects) &&
                      (rc.right - rc.left) >= 2 && (rc.bottom - rc.top) >= 2;  // HTML 版 0.02px，C++ 用 2px 防 0 尺寸
        if (!expand) {
            if (node->size > 0 || node->isDir) out.push_back({ node, rc, depth, lid });
            return;
        }
        std::vector<ScanNode*> kids;
        for (auto* c : node->children)
            if (c && (c->size > 0 || c->isDir)) kids.push_back(c);
        if (kids.empty()) {
            out.push_back({ node, rc, depth, lid });
            return;
        }
        std::stable_sort(kids.begin(), kids.end(),
                         [](const ScanNode* a, const ScanNode* b) {
                             if (a->isDir != b->isDir) return a->isDir > b->isDir;  // 目录优先
                             return a->size > b->size;                              // 同级 size 降序
                         });
        size_t kidsCap = (size_t)st.kidsCap;   // 0 = 每层不限
        if (kidsCap > 0 && kids.size() > kidsCap) kids.resize(kidsCap);
        int W = rc.right - rc.left, H = rc.bottom - rc.top;
        int subLid = nextLid++;
        std::vector<TreemapItem> sub;
        ComputeTreemap(kids, W, H, kidsCap, sub, A, subLid);
        for (const auto& it : sub) {
            RECT r = it.rc;
            r.left += rc.left; r.right += rc.left;
            r.top += rc.top; r.bottom += rc.top;
            rec(it.node, depth + 1, r, subLid);
        }
    };

    for (const auto& it : tops) rec(it.node, 0, it.rc, 0);

    // 布局后处理：与 Web 版（web/index.html squarify）完全一致 —— 不做任何
    // 最小高度强制（那会把小矩形拉高成细长条）。文字只在块内足够大时绘制。
}

// 扩展名 -> 类型分组
static int CategoryOf(const std::wstring& ext) {
    // 0 图片 1 视频 2 音频 3 文档 4 压缩 5 代码 6 其他
    static const struct {
        const wchar_t* ext;
        int cat;
    } map[] = {
        { L".jpg", 0 },  { L".jpeg", 0 }, { L".png", 0 }, { L".gif", 0 }, { L".bmp", 0 },
        { L".webp", 0 }, { L".ico", 0 },  { L".svg", 0 }, { L".tif", 0 }, { L".tiff", 0 },
        { L".mp4", 1 },  { L".avi", 1 },  { L".mkv", 1 }, { L".mov", 1 }, { L".wmv", 1 },
        { L".flv", 1 },  { L".webm", 1 }, { L".mp3", 2 }, { L".wav", 2 }, { L".flac", 2 },
        { L".ogg", 2 },  { L".m4a", 2 },  { L".aac", 2 }, { L".doc", 3 }, { L".docx", 3 },
        { L".pdf", 3 },  { L".txt", 3 },  { L".md", 3 },  { L".xls", 3 }, { L".xlsx", 3 },
        { L".ppt", 3 },  { L".pptx", 3 }, { L".csv", 3 }, { L".zip", 4 }, { L".rar", 4 },
        { L".7z", 4 },   { L".tar", 4 },  { L".gz", 4 },  { L".bz2", 4 }, { L".xz", 4 },
        { L".jar", 4 },  { L".war", 4 },  { L".cpp", 5 }, { L".cc", 5 },  { L".h", 5 },
        { L".hpp", 5 },  { L".c", 5 },    { L".java", 5 },{ L".py", 5 },  { L".js", 5 },
        { L".ts", 5 },   { L".json", 5 }, { L".xml", 5 }, { L".yml", 5 }, { L".yaml", 5 },
        { L".html", 5 }, { L".css", 5 },  { L".go", 5 },  { L".rs", 5 },  { L".kt", 5 },
    };
    for (const auto& m : map)
        if (ext == m.ext) return m.cat;
    return 6;
}

COLORREF TreemapColor(const ScanNode* node, int scheme) {
    if (node->isDir) return TC().treeDir;
    if (scheme == 2) return RGB(122, 130, 140);  // 单色
    if (scheme == 1) {                            // 按类型分组
        static const COLORREF cats[] = {
            RGB(46, 204, 113),   // 图片
            RGB(155, 89, 182),   // 视频
            RGB(230, 126, 34),   // 音频
            RGB(52, 152, 219),   // 文档
            RGB(231, 76, 60),    // 压缩
            RGB(26, 188, 156),   // 代码
            RGB(149, 165, 166),  // 其他
        };
        return cats[CategoryOf(ExtOf(node->name))];
    }
    // scheme 0：按扩展名哈希调色板
    static const COLORREF palette[] = {
        RGB(230, 126, 34),  RGB(231, 76, 60),   RGB(46, 204, 113), RGB(241, 196, 15),
        RGB(155, 89, 182),  RGB(52, 152, 219),  RGB(26, 188, 156), RGB(192, 57, 43),
        RGB(243, 156, 18),  RGB(149, 165, 166), RGB(39, 174, 96),  RGB(41, 128, 185),
        RGB(142, 68, 173),  RGB(236, 112, 99),
    };
    std::wstring ext = ExtOf(node->name);
    size_t h = 0;
    for (wchar_t c : ext) h = h * 131 + (size_t)c;
    return palette[h % (sizeof(palette) / sizeof(palette[0]))];
}

// ================= Win32 窗口 =================

namespace {

struct WheelEntry { ScanNode* hover; int idx; ScanNode* sel; };  // 上滚前状态快照
struct TMData {
    std::vector<ScanNode*> items;
    ScanNode* current = nullptr;
    std::vector<TreemapItem> layout;
    ScanNode* selected = nullptr;
    ScanNode* hoverNode = nullptr;
    ScanNode* blockMask = nullptr;       // 单块蒙版：仅该节点的矩形保持高亮
    bool blockMaskPresent = false;       // 该节点在当前布局中有矩形
    std::vector<WheelEntry> wheelStack;  // 上滚逐层历史，下滚逐级回退
    bool parentFrame = false;            // 父级框选态：整图压暗 + 父级区域框
    RECT parentRect{ 0, 0, 0, 0 };       // 父级区域并集（鼠标移出即恢复普通态）
    int hoverIdx = -1;
    POINT hoverPt{ -1, -1 };
    ULONGLONG lastWheelTick = 0;  // 最近一次滚轮框选时刻（屏蔽随后的双击误判）
    int W = 0, H = 0;
    bool dirty = true;
    // 布局后台计算（大目录全量布局不能卡 UI 线程）
    std::atomic<bool> layoutBusy{ false };
    int layoutGen = 0;       // 每次触发重算 +1；旧代际结果丢弃
    bool layoutReady = false;  // 布局是否已产出（首次 false → 画"计算中"）
    double scale = 1.0;  // DPI 缩放（96dpi 基准）

    HDC memDC = nullptr;
    HBITMAP memBmp = nullptr;
    int bmpW = 0, bmpH = 0;
    HFONT font = nullptr;
    HFONT fontSmall = nullptr;  // 第二行（大小/占比）与极小矩形用的小字号

    AppSettings st;
};TMData* Data(HWND hwnd) { return (TMData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA); }

COLORREF Lighten(COLORREF c) {
    return RGB(GetRValue(c) + (255 - GetRValue(c)) * 35 / 100,
               GetGValue(c) + (255 - GetGValue(c)) * 35 / 100,
               GetBValue(c) + (255 - GetBValue(c)) * 35 / 100);
}

static COLORREF Mix(COLORREF a, COLORREF b, double t) {
    return RGB((int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t),
               (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t),
               (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t));
}

// ============ Direct2D 线性渐变（GPU 逐像素平滑插值） ============
static ID2D1Factory* g_d2df = nullptr;
static ID2D1DCRenderTarget* g_d2drt = nullptr;
static std::map<COLORREF, ID2D1LinearGradientBrush*> g_d2dBr;  // 按基底色缓存渐变刷
static bool g_d2dOK = false;

static D2D1_COLOR_F D2DColor(COLORREF c) {
    return D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f,
                        1.0f);
}

static void EnsureD2D() {
    if (g_d2df) return;
    g_d2dOK = SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2df));
    if (g_d2dOK) {
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        g_d2dOK = SUCCEEDED(g_d2df->CreateDCRenderTarget(&props, &g_d2drt));
    }
}

static ID2D1LinearGradientBrush* GetD2DBrush(COLORREF c) {
    auto it = g_d2dBr.find(c);
    if (it != g_d2dBr.end()) return it->second;
    if (!g_d2df || !g_d2drt) return nullptr;
    // 线性渐变：0=基底色，1=提亮 40%（与旧径向/对角强度一致），单位起点(0,0)→终点(1,1)
    D2D1_GRADIENT_STOP gs[2] = { { 0.0f, D2DColor(c) },
                                 { 1.0f, D2DColor(Mix(c, RGB(255, 255, 255), 0.4)) } };
    ID2D1GradientStopCollection* stops = nullptr;
    if (FAILED(g_d2drt->CreateGradientStopCollection(gs, 2, &stops))) return nullptr;
    ID2D1LinearGradientBrush* br = nullptr;
    g_d2drt->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(1, 1)), stops,
        &br);
    stops->Release();
    if (br) g_d2dBr[c] = br;
    return br;
}

// 用渐变刷填充矩形：刷坐标(0,0)→(1,1) 经缩放平移映射到矩形，方向统一 左上→右下
static bool D2DFillGradient(const RECT& r, COLORREF c) {
    ID2D1LinearGradientBrush* br = GetD2DBrush(c);
    if (!br) return false;
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return true;
    br->SetTransform(D2D1::Matrix3x2F::Scale((float)w, (float)h) *
                     D2D1::Matrix3x2F::Translation((float)r.left, (float)r.top));
    g_d2drt->FillRectangle(
        D2D1::RectF((float)r.left, (float)r.top, (float)r.right, (float)r.bottom), br);
    return true;
}

static void ReleaseD2D() {
    for (auto& kv : g_d2dBr) if (kv.second) kv.second->Release();
    g_d2dBr.clear();
    if (g_d2drt) { g_d2drt->Release(); g_d2drt = nullptr; }
    if (g_d2df) { g_d2df->Release(); g_d2df = nullptr; }
    g_d2dOK = false;
}

// GDI 分段渐变（D2D 不可用时的回退路径）
static void DrawGradientRect(HDC dc, const RECT& rc, COLORREF base) {
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    if (w < 3 || h < 3) {  // 极小方块：纯色，渐变无意义
        SetDCBrushColor(dc, base);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(DC_BRUSH));
        return;
    }
    const double strength = 0.4;
    // 段数随宽度自适应：大块细分到 48 段（段宽≈宽/48，色带不可见），小块最少 12 段
    int segs = w < 12 ? w : std::min(48, std::max(12, w / 4));
    for (int y = rc.top; y < rc.bottom; y++) {
        double fy = (double)(y - rc.top) / h;
        for (int s = 0; s < segs; s++) {
            int x0 = rc.left + w * s / segs;
            int x1 = rc.left + w * (s + 1) / segs;
            if (x1 <= x0) continue;
            double fx = ((double)s + 0.5) / segs;
            double t = (fx + fy) * 0.5;  // 统一：左上→右下
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            COLORREF c = Mix(base, RGB(255, 255, 255), t * strength);
            SetDCBrushColor(dc, c);
            RECT rr{ x0, y, x1, y + 1 };
            FillRect(dc, &rr, (HBRUSH)GetStockObject(DC_BRUSH));
        }
    }
}

bool IsDescendantOf(const ScanNode* node, const ScanNode* anc) {
    for (const ScanNode* p = node; p; p = p->parent)
        if (p == anc) return true;
    return false;
}

// 选中项包围矩形：自身矩形或其展开子矩形并集（用于悬浮信息定位）
bool SelectionRect(TMData* d, RECT* out) {
    if (!d->selected) return false;
    for (const auto& it : d->layout)
        if (it.node == d->selected) {
            *out = it.rc;
            return true;
        }
    RECT u{};
    bool any = false;
    for (const auto& it : d->layout) {
        if (IsDescendantOf(it.node, d->selected)) {
            if (any) UnionRect(&u, &u, &it.rc);
            else u = it.rc;
            any = true;
        }
    }
    if (any) {
        *out = u;
        return true;
    }
    return false;
}

void EnsureBuffer(TMData* d, HDC refDC) {
    if (d->memDC && d->bmpW == d->W && d->bmpH == d->H) return;
    if (d->memDC) {
        DeleteObject(d->memBmp);
        DeleteDC(d->memDC);
        d->memDC = nullptr;
        d->memBmp = nullptr;
    }
    d->memDC = CreateCompatibleDC(refDC);
    d->memBmp = CreateCompatibleBitmap(refDC, d->W, d->H);
    SelectObject(d->memDC, d->memBmp);
    d->bmpW = d->W;
    d->bmpH = d->H;
}

// 沿 parent 链拼接完整路径
static std::wstring BuildPath(const ScanNode* n) {
    std::vector<const ScanNode*> chain;
    for (const ScanNode* p = n; p; p = p->parent) chain.push_back(p);
    std::wstring s;
    for (size_t i = chain.size(); i-- > 0;) {
        if (!chain[i]->name.empty()) {
            if (!s.empty()) s += L'\\';
            s += chain[i]->name;
        }
    }
    return s;
}

void DrawHoverPanel(TMData* d) {
    if (!d->hoverNode) return;  // 父级框选态：hoverIdx=-1 但 hoverNode=父级，面板照常显示
    HDC dc = d->memDC;
    double sc = d->scale;

    std::wstring path = BuildPath(d->hoverNode);
    if (path.empty()) path = d->hoverNode->name;
    ULONGLONG total = d->current ? d->current->size : 0;
    double pct = total ? (double)d->hoverNode->size / (double)total * 100.0 : 0.0;
    wchar_t pctBuf[32];
    swprintf(pctBuf, 32, L"%.1f%%", pct);
    std::wstring line =
        d->hoverNode->name + L"\n" + path + L"\n" + FormatSize(d->hoverNode->size) +
        L"  (占比 " + pctBuf + L")";

    HFONT oldFont = (HFONT)SelectObject(dc, d->font);
    RECT m{};
    DrawTextW(dc, line.c_str(), -1, &m, DT_CALCRECT | DT_NOPREFIX);
    int padX = (int)(11 * sc + 0.5), padY = (int)(9 * sc + 0.5);
    int pw = m.right - m.left + padX * 2, ph = m.bottom - m.top + padY * 2;
    POINT pos{ d->hoverPt.x + (int)(14 * sc + 0.5), d->hoverPt.y + (int)(16 * sc + 0.5) };
    int gap = (int)(8 * sc + 0.5);
    if (pos.x + pw > d->W) pos.x = d->hoverPt.x - pw - gap;
    if (pos.y + ph > d->H) pos.y = d->hoverPt.y - ph - gap;
    int mgn = (int)(2 * sc + 0.5);
    if (pos.x < mgn) pos.x = mgn;
    if (pos.y < mgn) pos.y = mgn;

    RECT panel{ pos.x, pos.y, pos.x + pw, pos.y + ph };
    SetDCBrushColor(dc, TC().panel);
    FillRect(dc, &panel, (HBRUSH)GetStockObject(DC_BRUSH));
    SetTextColor(dc, TC().text);
    SetBkMode(dc, TRANSPARENT);
    RECT tr{ pos.x + padX, pos.y + padY, pos.x + pw - padX, pos.y + ph - padY };
    DrawTextW(dc, line.c_str(), -1, &tr, DT_LEFT | DT_TOP | DT_NOPREFIX);
    SelectObject(dc, oldFont);
}

#define WM_APP_TREEMAP_LAYOUT (WM_APP + 40)   // 后台布局完成（wParam=gen, lParam=vector*）

struct LayoutJob {
    HWND hwnd;
    int gen;
    std::vector<ScanNode*> items;   // 浅拷贝（节点由结果树持有，稳定）
    int W, H;
    AppSettings st;
};

static DWORD WINAPI LayoutThreadProc(LPVOID p) {
    std::unique_ptr<LayoutJob> job((LayoutJob*)p);
    std::vector<TreemapItem> out;
    ComputeTreemapRecursive(job->items, job->W, job->H, job->st, out);
    auto* box = new std::vector<TreemapItem>(std::move(out));
    if (!PostMessageW(job->hwnd, WM_APP_TREEMAP_LAYOUT, (WPARAM)job->gen, (LPARAM)box))
        delete box;
    return 0;
}

static void LaunchLayout(TMData* d, HWND hwnd) {
    if (d->layoutBusy.load()) return;
    int gen = ++d->layoutGen;
    d->layoutBusy.store(true);
    auto* job = new LayoutJob{ hwnd, gen, d->items, d->W, d->H, d->st };
    HANDLE h = CreateThread(nullptr, 0, LayoutThreadProc, job, 0, nullptr);
    if (h) CloseHandle(h);
    else {
        d->layoutBusy.store(false);
        delete job;
    }
}

void Render(TMData* d, HWND hwnd) {
    HDC dc = d->memDC;
    RECT whole{ 0, 0, d->W, d->H };
    SetDCBrushColor(dc, TC().treeBg);
    FillRect(dc, &whole, (HBRUSH)GetStockObject(DC_BRUSH));

    if (!d->current || d->items.empty()) {
        SetTextColor(dc, RGB(132, 138, 146));
        SetBkMode(dc, TRANSPARENT);
        RECT tr{ 8, 8, d->W - 8, d->H - 8 };
        DrawTextW(dc, L"暂无数据 - 输入路径点击「扫描」", -1, &tr, DT_LEFT | DT_TOP);
        return;
    }

    // 布局后台计算：dirty（数据/设置/尺寸变化）→ 起线程重算；首次产出前画占位
    if (d->dirty) {
        LaunchLayout(d, hwnd);
        if (!d->layoutReady) {
            SetTextColor(dc, RGB(132, 138, 146));
            SetBkMode(dc, TRANSPARENT);
            RECT tr{ 8, 8, d->W - 8, d->H - 8 };
            DrawTextW(dc, L"正在计算布局…", -1, &tr, DT_LEFT | DT_TOP);
            return;
        }
        // layoutReady=true：继续用旧布局绘制，新布局到达后自动重绘
    }

    // ---- 色块渐变：优先 Direct2D（GPU 逐像素平滑），失败回退 GDI 分段 ----
    EnsureD2D();
    bool d2d = false;
    if (g_d2dOK && g_d2drt) {
        RECT rc{ 0, 0, d->W, d->H };
        if (SUCCEEDED(g_d2drt->BindDC(dc, &rc))) {
            g_d2drt->BeginDraw();
            d2d = true;
        }
    }
    auto FillRect = [&](const RECT& r, COLORREF c) {
        if (d2d) D2DFillGradient(r, c);
        else DrawGradientRect(dc, r, c);
    };

    // 单块蒙版（列表行联动的色块级高亮）：仅命中节点的矩形保持原色，其余压暗
    bool maskDim = d->blockMask && d->blockMaskPresent;
    bool maskHit = false;
    if (maskDim) {
        for (const auto& it : d->layout) {
            if (IsDescendantOf(it.node, d->blockMask)) { maskHit = true; break; }
        }
        if (!maskHit) maskDim = false;
    }

    // 基础色块（矩形完全贴合，零间隙、零边框）；父级框选态下整图压暗
    // 每个矩形膨胀 1px 再填：消除相邻矩形取整造成的 1px 缝隙（深色背景露出 → 黑线）
    bool anyHighlighted = false;
    for (const auto& it : d->layout) {
        RECT r = it.rc;
        if (r.right <= r.left || r.bottom <= r.top) continue;
        r.left--; r.top--; r.right++; r.bottom++;
        if (r.left < 0) r.left = 0;
        if (r.top < 0) r.top = 0;
        if (r.right > d->W) r.right = d->W;
        if (r.bottom > d->H) r.bottom = d->H;
        bool inMask = !maskDim || IsDescendantOf(it.node, d->blockMask);
        if (inMask) anyHighlighted = true;
        COLORREF c = TreemapColor(it.node, d->st.colorScheme);
        if (d->parentFrame) c = Mix(c, RGB(0, 0, 0), 0.12);
        if (maskDim && !inMask) c = Mix(c, RGB(0, 0, 0), 0.55);
        FillRect(r, c);
    }

    // 悬停高亮：提亮渐变填充（边框随后在 D2D 结束后用 GDI 画）
    if (d->hoverIdx >= 0 && d->hoverIdx < (int)d->layout.size()) {
        RECT r = d->layout[d->hoverIdx].rc;
        if (r.right > r.left && r.bottom > r.top) {
            r.left--; r.top--; r.right++; r.bottom++;
            if (r.left < 0) r.left = 0;
            if (r.top < 0) r.top = 0;
            if (r.right > d->W) r.right = d->W;
            if (r.bottom > d->H) r.bottom = d->H;
            FillRect(r, Lighten(TreemapColor(d->layout[d->hoverIdx].node, d->st.colorScheme)));
        }
    }

    // 选中高亮：对选中节点全部矩形双重提亮渐变填充（无边框）
    if (d2d) g_d2drt->EndDraw();

    // ---- GDI 覆盖层：标签文字 + 悬浮边框（D2D 结束后绘制） ----
    auto DrawBlockText = [&](const RECT& r, const ScanNode* node) {
        if (r.right <= r.left || r.bottom <= r.top) return;
        double total = d->current ? (double)d->current->size : 0.0;
        const wchar_t* nm = node->name.c_str();
        int nh = r.bottom - r.top;
        int tw = r.right - r.left;
        bool dotted = tw < 40 || (nh >= (int)(40 * d->scale) && tw < 60);
        std::wstring sizeLine = FormatSize(node->size);
        if (total > 0) {
            wchar_t pb[32];
            swprintf(pb, 32, L"%.1f%%", (double)node->size / total * 100.0);
            sizeLine += L" · ";
            sizeLine += pb;
        }
        HFONT oldF = nullptr;
        if (dotted) {
            oldF = (HFONT)SelectObject(dc, d->fontSmall);
        } else if (nh >= (int)(26 * d->scale)) {
            // 名称在上一行（更大字号 + 纯度更高的白），大小/占比在下一行（更小字号 + 暗色）
            oldF = (HFONT)SelectObject(dc, d->font);
            SetTextColor(dc, RGB(255, 255, 255));
            SetBkMode(dc, TRANSPARENT);
            RECT tr = r;
            tr.left += 4; tr.top += 2; tr.right -= 4; tr.bottom = tr.top + (int)(16 * d->scale);
            DrawTextW(dc, nm, -1, &tr,
                      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            HFONT of2 = (HFONT)SelectObject(dc, d->fontSmall);
            SetTextColor(dc, RGB(246, 249, 252));
            tr.top = tr.bottom;
            tr.bottom = r.bottom - 1;
            DrawTextW(dc, sizeLine.c_str(), -1, &tr,
                      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(dc, of2);
            SelectObject(dc, oldF);
            return;
        } else {
            oldF = (HFONT)SelectObject(dc, d->fontSmall);
            SetTextColor(dc, RGB(240, 242, 245));
            SetBkMode(dc, TRANSPARENT);
            RECT tr = r;
            tr.left += 4; tr.top += 2; tr.right -= 4; tr.bottom -= 2;
            DrawTextW(dc, nm, -1, &tr,
                      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(dc, oldF);
            return;
        }
        // 极小矩形：单行「名称 · 大小 · 占比」，用小字号
        SetBkMode(dc, TRANSPARENT);
        RECT tr = r;
        tr.left += 2; tr.top += 1; tr.right -= 2; tr.bottom -= 1;
        std::wstring one = nm;
        one += L" · ";
        one += sizeLine;
        wchar_t probe[512];
        lstrcpynW(probe, one.c_str(), 512);
        SetTextColor(dc, RGB(250, 252, 255));
        DrawTextW(dc, probe, -1, &tr, DT_LEFT | DT_TOP | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, oldF);
    };

    for (const auto& it : d->layout) {
        RECT r = it.rc;
        if (r.right <= r.left || r.bottom <= r.top) continue;
        if (!d->st.showLabels) break;
        if (r.right - r.left >= 26 && r.bottom - r.top >= 8) DrawBlockText(r, it.node);
    }
    if (d->hoverIdx >= 0 && d->hoverIdx < (int)d->layout.size()) {
        RECT r = d->layout[d->hoverIdx].rc;
        if (r.right > r.left && r.bottom > r.top) {
            SetDCBrushColor(dc, RGB(214, 226, 240));
            FrameRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
        }
    }

    // 选中焦点框：框住选中节点（及其后代）在当前视图的并集边界，
    // 轻量联动（不重刷色块），双向绑定只在焦点框层面
    if (d->selected) {
        RECT ur{ INT_MAX, INT_MAX, INT_MIN, INT_MIN };
        bool any = false;
        for (const auto& it : d->layout) {
            bool belongs = false;
            for (ScanNode* q = it.node; q; q = q->parent) {
                if (q == d->selected) { belongs = true; break; }
            }
            if (!belongs) continue;
            any = true;
            if (it.rc.left < ur.left) ur.left = it.rc.left;
            if (it.rc.top < ur.top) ur.top = it.rc.top;
            if (it.rc.right > ur.right) ur.right = it.rc.right;
            if (it.rc.bottom > ur.bottom) ur.bottom = it.rc.bottom;
        }
        if (any && ur.right > ur.left && ur.bottom > ur.top) {
            HPEN pen = CreatePen(PS_SOLID, (int)(2 * d->scale + 0.5), RGB(255, 176, 64));
            HGDIOBJ op = SelectObject(dc, pen);
            HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, ur.left, ur.top, ur.right, ur.bottom);
            SelectObject(dc, op);
            SelectObject(dc, ob);
            DeleteObject(pen);
        }
    }

    // 父级框：框住父级目录在当前视图中占据的区域并集（parentRect），
    // 面板与框指向同一个目录；鼠标移出该区域即恢复普通态
    if (d->parentFrame && d->parentRect.right > d->parentRect.left &&
        d->parentRect.bottom > d->parentRect.top) {
        RECT ur = d->parentRect;
        HPEN pen = CreatePen(PS_SOLID, (int)(2 * d->scale + 0.5), RGB(214, 226, 240));
        HGDIOBJ op = SelectObject(dc, pen);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, ur.left, ur.top, ur.right, ur.bottom);
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(pen);
    }

    // 单块蒙版：只给命中节点的矩形描白边（用色块本身标出目标，不额外框整块）
    if (maskDim && anyHighlighted) {
        SetDCBrushColor(dc, RGB(232, 240, 248));
        for (const auto& it : d->layout) {
            if (!IsDescendantOf(it.node, d->blockMask)) continue;
            FrameRect(dc, &it.rc, (HBRUSH)GetStockObject(DC_BRUSH));
        }
    }

    // 悬浮信息面板（最后绘制，浮在最上层）
    DrawHoverPanel(d);
}

int HitTest(TMData* d, POINT pt) {
    for (size_t i = 0; i < d->layout.size(); i++)
        if (PtInRect(&d->layout[i].rc, pt)) return (int)i;
    return -1;
}

// 父级目录（及其后代）在当前视图中占据的所有矩形的并集边界；无命中时整画布
static void ComputeParentRect(TMData* d, ScanNode* p, RECT& out) {
    out = { INT_MAX, INT_MAX, INT_MIN, INT_MIN };
    bool any = false;
    for (const auto& it : d->layout) {
        bool belongs = false;
        for (ScanNode* q = it.node; q; q = q->parent) {
            if (q == p) { belongs = true; break; }
        }
        if (!belongs) continue;
        any = true;
        if (it.rc.left < out.left) out.left = it.rc.left;
        if (it.rc.top < out.top) out.top = it.rc.top;
        if (it.rc.right > out.right) out.right = it.rc.right;
        if (it.rc.bottom > out.bottom) out.bottom = it.rc.bottom;
    }
    if (!any) out = { 0, 0, d->W, d->H };
}

LRESULT CALLBACK TreeMapWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            auto* d = new TMData();
            HDC sdc = GetDC(hwnd);
            d->scale = GetDeviceCaps(sdc, LOGPIXELSX) / 96.0;
            ReleaseDC(hwnd, sdc);
            if (d->scale < 1.0) d->scale = 1.0;
            d->font = CreateFontW(-(int)(15 * d->scale + 0.5), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                  DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            d->fontSmall = CreateFontW(-(int)(11 * d->scale + 0.5), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                       DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)d);
            return 0;
        }
        case WM_SIZE: {
            TMData* d = Data(hwnd);
            d->W = LOWORD(lParam);
            d->H = HIWORD(lParam);
            d->dirty = true;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            TMData* d = Data(hwnd);
            if (d) {
                EnsureBuffer(d, hdc);
                // 每次全量重渲染：hover/选中/滚轮等交互态直接反映到画布
                Render(d, hwnd);
                BitBlt(hdc, 0, 0, d->W, d->H, d->memDC, 0, 0, SRCCOPY);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            TMData* d = Data(hwnd);
            if (!d) break;
            // 悬浮即抢焦点：滚轮事件发给焦点窗口，焦点在列表/编辑框时树图收不到
            if (GetFocus() != hwnd) SetFocus(hwnd);
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            // 父级框选态：框内保持父级信息（面板跟随）；移出框立即恢复正常 hover
            if (d->parentFrame) {
                if (!PtInRect(&d->parentRect, pt)) {
                    d->parentFrame = false;
                    if (!d->wheelStack.empty()) {
                        WheelEntry base2 = d->wheelStack.front();  // 恢复最初状态
                        d->wheelStack.clear();
                        d->hoverNode = base2.hover;
                        d->hoverIdx = base2.idx;
                        d->selected = base2.sel;
                    }
                    int idx = HitTest(d, pt);  // 跟随鼠标当前位置的色块
                    d->hoverIdx = idx;
                    d->hoverNode = (idx >= 0) ? d->layout[idx].node : nullptr;
                    d->hoverPt = pt;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (abs(pt.x - d->hoverPt.x) > 1 || abs(pt.y - d->hoverPt.y) > 1) {
                    d->hoverPt = pt;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            int idx = HitTest(d, pt);
            // 进入新块，或指针移动超过 1px（悬浮面板跟随）时重绘
            bool moved = abs(pt.x - d->hoverPt.x) > 1 || abs(pt.y - d->hoverPt.y) > 1;
            if (idx != d->hoverIdx || moved) {
                d->hoverIdx = idx;
                d->hoverNode = (idx >= 0) ? d->layout[idx].node : nullptr;
                d->hoverPt = pt;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (idx >= 0) {
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
            }
            return 0;
        }
        case WM_MOUSELEAVE: {
            TMData* d = Data(hwnd);
            if (d && d->hoverIdx >= 0) {
                d->hoverIdx = -1;
                d->hoverNode = nullptr;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {  // 单击=仅选中 + 双向绑定：通知主窗口定位到列表
            TMData* d = Data(hwnd);
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            int idx = HitTest(d, pt);
            d->selected = (idx >= 0) ? d->layout[idx].node : nullptr;
            d->hoverIdx = idx;
            d->hoverNode = (idx >= 0) ? d->layout[idx].node : nullptr;
            d->hoverPt = pt;
            InvalidateRect(hwnd, nullptr, FALSE);
            // 上滚框选后 500ms 内：只选中，不联动定位（避免"上滚后点一下就跳走"）
            if (GetTickCount64() - d->lastWheelTick < 500) return 0;
            if (idx >= 0 && d->layout[idx].node)
                PostMessageW(GetParent(hwnd), WM_APP_TREEMAP_LOCATE,
                             (WPARAM)d->layout[idx].node, 0);
            return 0;
        }
        case WM_LBUTTONDBLCLK: {  // 双击=进入目录 / 打开文件
            TMData* d = Data(hwnd);
            // 上滚框选后 500ms 内的双击实为单击误判：只选中，不进入
            if (GetTickCount64() - d->lastWheelTick < 500) {
                POINT pt0{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                int i0 = HitTest(d, pt0);
                if (i0 >= 0) {
                    d->selected = d->layout[i0].node;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            int idx = HitTest(d, pt);
            if (idx >= 0) {
                ScanNode* n = d->layout[idx].node;
                if (n) PostMessageW(GetParent(hwnd), WM_APP_TREEMAP_NAV, 0, (LPARAM)n);
            }
            return 0;
        }
        case WM_RBUTTONUP: {
            TMData* d = Data(hwnd);
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            int idx = HitTest(d, pt);
            ScanNode* n = (idx >= 0) ? d->layout[idx].node : nullptr;
            if (n) d->selected = n;  // 右键同时选中
            POINT sp = pt;
            ClientToScreen(hwnd, &sp);
            PostMessageW(GetParent(hwnd), WM_APP_TREEMAP_MENU, (WPARAM)n,
                         MAKELPARAM(sp.x, sp.y));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEWHEEL: {  // 上滚=逐层框选更上层父目录，下滚=逐级回退
            TMData* d = Data(hwnd);
            short delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (delta > 0) {
                d->lastWheelTick = GetTickCount64();  // 上滚框选：屏蔽随后的双击误判
                ScanNode* base = d->hoverNode ? d->hoverNode : d->current;
                if (!base) return 0;
                // 悬浮在色块/父级态 → 父级=其父（逐层上跳）；无悬浮空白 → 父级=当前目录自身
                ScanNode* p = d->hoverNode ? base->parent : base;
                if (!p) p = base;
                if (p == d->hoverNode) return 0;  // 已到根/自身，无更上层
                d->wheelStack.push_back({ d->hoverNode, d->hoverIdx, d->selected });
                d->selected = p;
                d->hoverNode = p;        // hover 焦点转移到父级
                d->hoverIdx = -1;        // 父级无单块框（框=父级区域并集）
                d->parentFrame = true;
                ComputeParentRect(d, p, d->parentRect);
            } else {
                if (!d->wheelStack.empty()) {
                    WheelEntry back = d->wheelStack.back();
                    d->wheelStack.pop_back();
                    d->hoverNode = back.hover;
                    d->hoverIdx = back.idx;
                    d->selected = back.sel;
                    d->parentFrame = !d->wheelStack.empty();
                    if (d->parentFrame)
                        ComputeParentRect(d, d->hoverNode, d->parentRect);
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_APP_TREEMAP_SELECT_PARENT: {
            TMData* d = Data(hwnd);
            if (d && d->selected && d->selected->parent) {
                d->selected = d->selected->parent;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_APP_TREEMAP_SELECT_PARENT_DIR: {
            TMData* d = Data(hwnd);
            ScanNode* cur = (ScanNode*)wParam;
            if (d && cur && cur->parent) {
                if (d->hoverNode != cur->parent) {
                    d->wheelStack.push_back({ d->hoverNode, d->hoverIdx, d->selected });
                    d->hoverNode = cur->parent;
                    d->hoverIdx = -1;
                    d->selected = cur->parent;
                    d->parentFrame = true;
                    ComputeParentRect(d, cur->parent, d->parentRect);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        }
        case WM_APP_TREEMAP_SELECT_NODE: {
            TMData* d = Data(hwnd);
            if (d && wParam) {
                ScanNode* n = (ScanNode*)wParam;
                if (d->selected != n) {  // 同节点不重绘
                    d->selected = n;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        }
        case WM_APP_TREEMAP_LAYOUT: {
            TMData* d = Data(hwnd);
            auto* box = (std::vector<TreemapItem>*)lParam;
            if (d) {
                if (wParam == (WPARAM)d->layoutGen) {
                    d->layout = std::move(*box);
                    d->layoutReady = true;
                    d->layoutBusy.store(false);
                    d->dirty = false;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                // 旧代际结果直接丢弃（新代际线程仍在跑，busy 保持）
            }
            delete box;
            return 0;
        }
        case WM_DESTROY: {
            TMData* d = Data(hwnd);
            if (d) {
                if (d->memDC) {
                    DeleteObject(d->memBmp);
                    DeleteDC(d->memDC);
                }
                if (d->font) DeleteObject(d->font);
                if (d->fontSmall) DeleteObject(d->fontSmall);
                delete d;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            }
            ReleaseD2D();  // 释放渐变刷 / D2D 设备
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

void TreemapRegister(HINSTANCE hInst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TreeMapWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DiskMateTreeMap";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExW(&wc);
}

HWND TreemapCreate(HWND parent, HINSTANCE hInst) {
    return CreateWindowExW(0, L"DiskMateTreeMap", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                           0, 0, 0, 0, parent, nullptr, hInst, nullptr);
}

void TreemapSetData(HWND hwnd, const std::vector<ScanNode*>& items, ScanNode* current) {
    TMData* d = Data(hwnd);
    if (!d) return;
    d->items = items;
    d->current = current;
    d->selected = nullptr;
    d->hoverIdx = -1;
    d->hoverNode = nullptr;
    d->blockMask = nullptr;
    d->blockMaskPresent = false;
    d->layoutReady = false;   // 新数据：旧布局作废，画"计算中"直到后台产出
    d->dirty = true;
    InvalidateRect(hwnd, nullptr, FALSE);
}

void TreemapSetBlockMask(HWND hwnd, ScanNode* node) {
    TMData* d = Data(hwnd);
    if (!d) return;
    if (d->blockMask == node) return;  // 同节点不重绘
    d->blockMask = node;
    d->blockMaskPresent = false;
    if (node) {
        for (const auto& it : d->layout) {
            if (IsDescendantOf(it.node, node)) { d->blockMaskPresent = true; break; }
        }
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

void TreemapSetSettings(HWND hwnd, const AppSettings& st) {
    TMData* d = Data(hwnd);
    if (!d) return;
    d->st = st;
    d->dirty = true;
    InvalidateRect(hwnd, nullptr, FALSE);
}
