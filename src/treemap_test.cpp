// treemap_test：树图布局自检（真实扫描数据）+ 渲染预览图（BMP）
// 验证递归布局：越界 / 重叠 / 覆盖率；并展示设置参数对矩形数的影响
// 用法: treemap_test [路径]   默认扫描本工程目录
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "scanner.h"
#include "settings.h"
#include "treemap.h"
#include "utils.h"

static void WriteBMP(const char* path, int W, int H, const std::vector<DWORD>& px) {
    BITMAPFILEHEADER bf{};
    BITMAPINFOHEADER bi{};
    bf.bfType = 0x4D42;
    bf.bfOffBits = sizeof(bf) + sizeof(bi);
    bf.bfSize = bf.bfOffBits + (DWORD)px.size() * 4;
    bi.biSize = sizeof(bi);
    bi.biWidth = W;
    bi.biHeight = H;
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biSizeImage = (DWORD)px.size() * 4;
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fwrite(&bf, sizeof(bf), 1, f);
    fwrite(&bi, sizeof(bi), 1, f);
    for (int y = H - 1; y >= 0; y--) fwrite(px.data() + (size_t)y * W, 4, W, f);
    fclose(f);
}

struct CheckResult {
    int badBounds = 0;
    ULONGLONG overlap = 0;
    ULONGLONG overlapNonFamily = 0;  // 同布局内、非父子关系的重叠（真正的 bug）
    double coverage = 0;
    double maxAR = 0;   // 全布局最大长宽比
    int overCount = 0;  // 长宽比 >3 的矩形数（应只来自最小项，数量最小）
    int reversed = 0;   // 面积反转：小文件矩形面积超过大文件 1%+ 的对数
    int minBlockH = 0;  // 最小块高（px）
    int lowBlocks = 0;  // 高度 < 40px（两行文字放不下）的块数
};

static bool IsAncestor(const ScanNode* a, const ScanNode* n) {
    for (const ScanNode* p = n; p; p = p->parent)
        if (p == a) return true;
    return false;
}

static CheckResult Check(const std::vector<TreemapItem>& out, int W, int H) {
    CheckResult r;
    ULONGLONG areaSum = 0;
    r.minBlockH = INT_MAX;
    for (const auto& a : out) {
        if (a.rc.left < 0 || a.rc.top < 0 || a.rc.right > W || a.rc.bottom > H) r.badBounds++;
        int w = a.rc.right - a.rc.left, h = a.rc.bottom - a.rc.top;
        areaSum += (ULONGLONG)w * h;
        double ar = (w >= h) ? (double)w / (double)h : (double)h / (double)w;
        r.maxAR = std::max(r.maxAR, ar);
        if (ar > 3.0001) r.overCount++;
        if (h < r.minBlockH) r.minBlockH = h;
        if (h < 40) r.lowBlocks++;
    }
    for (size_t i = 0; i < out.size(); i++)
        for (size_t j = i + 1; j < out.size(); j++) {
            const RECT& a = out[i].rc;
            const RECT& b = out[j].rc;
            int w = std::min(a.right, b.right) - std::max(a.left, b.left);
            int h = std::min(a.bottom, b.bottom) - std::max(a.top, b.top);
            if (w > 0 && h > 0) {
                r.overlap += (ULONGLONG)w * h;
                // 同布局 + 非父子：这是像素再分配必须保证「零新增」的口径
                if (out[i].layoutId == out[j].layoutId && out[i].node && out[j].node &&
                    !IsAncestor(out[i].node, out[j].node) &&
                    !IsAncestor(out[j].node, out[i].node)) {
                    r.overlapNonFamily += (ULONGLONG)w * h;
                    if (r.overlapNonFamily <= (ULONGLONG)w * h && r.overlapNonFamily < 6) {
                        wprintf(L"  非父子重叠: %ls(%d,%d,%d,%d) ∩ %ls(%d,%d,%d,%d) = %dx%d\n",
                                out[i].node->name.c_str(), a.left, a.top, a.right, a.bottom,
                                out[j].node->name.c_str(), b.left, b.top, b.right, b.bottom, w, h);
                    }
                }
            }
        }
    if (out.empty()) r.minBlockH = 0;
    // 面积反转：仅比较"同一布局实例"项（同 layoutId，面积严格 ∝ log2(size)，浮动 ≤1%）。
    // 跨布局（不同父矩形）的绝对面积差异是嵌套树图的固有语义（父矩形占比×局部占比），不计入。
    for (size_t i = 0; i < out.size(); i++)
        for (size_t j = 0; j < out.size(); j++) {
            if (i == j || !out[i].node || !out[j].node) continue;
            if (out[i].layoutId != out[j].layoutId) continue;
            if (out[i].node->size >= out[j].node->size) continue;
            int wi = out[i].rc.right - out[i].rc.left, hi = out[i].rc.bottom - out[i].rc.top;
            int wj = out[j].rc.right - out[j].rc.left, hj = out[j].rc.bottom - out[j].rc.top;
            double ai = (double)wi * hi, aj = (double)wj * hj;
            if (ai > aj * 1.01) {
                r.reversed++;
                if (r.reversed <= 6)
                    wprintf(L"  反转: %ls(%lluB,面积%.0f) > %ls(%lluB,面积%.0f)  矩形宽高(%d,%d) vs (%d,%d)\n",
                            out[i].node->name.c_str(), out[i].node->size, ai,
                            out[j].node->name.c_str(), out[j].node->size, aj, wi, hi, wj, hj);
            }
        }
    r.coverage = (double)areaSum / (W * H) * 100.0;
    return r;
}

int wmain(int argc, wchar_t** argv) {
    setlocale(LC_ALL, "");

    std::wstring root =
        (argc > 1) ? NormalizeRoot(argv[1])
                   : L"C:\\Users\\Jacob\\Doubao\\chats\\2026-10-04\\new-chat\\diskmate";
    if (!IsDirPath(root)) {
        wprintf(L"路径无效: %ls\n", root.c_str());
        return 2;
    }

    std::atomic<bool> cancel(false);
    ScanStats stats;
    auto t0 = std::chrono::steady_clock::now();
    ScanResult res = ScanTree(root, cancel, &stats, nullptr);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0)
                  .count();
    wprintf(L"扫描: %ls  (%llu 项, %ls, %lld ms)\n", root.c_str(), stats.items,
            FormatSize(stats.bytes).c_str(), ms);

    std::vector<ScanNode*> items = res.root->children;
    const int W = 800, H = 600;

    // 参数化演示：不同设置下矩形数
    auto mk = [](int depth, int minMB, int rects) {
        AppSettings s;
        s.maxDepth = depth;
        s.minSizeMB = minMB;
        s.maxRects = rects;
        return s;
    };
    const AppSettings combos[] = {
        mk(10, 1, 2000),   // 默认：递归到 <1MB
        mk(10, 0, 2000),   // 最小尺寸=0：尽量全展开
        mk(10, 100, 2000), // 最小尺寸=100MB：几乎不展开
        mk(1, 1, 2000),    // 层级=1：只展开一层
        mk(10, 1, 500),    // 矩形数上限=500
    };
    for (const auto& c : combos) {
        std::vector<TreemapItem> out;
        ComputeTreemapRecursive(items, W, H, c, out);
        CheckResult r = Check(out, W, H);
        wprintf(L"设置(层级%d,最小%3dMB,上限%5d) -> 矩形 %5zu  越界 %d  重叠 %llu(非父子 %llu)  覆盖 %.2f%%  "
                L"长宽比最大 %.1f  超限(>3) %d 个  面积反转 %d 对  最小块高 %dpx  <40px 块 %d\n",
                c.maxDepth, c.minSizeMB, c.maxRects, out.size(), r.badBounds, r.overlap,
                r.overlapNonFamily, r.coverage, r.maxAR, r.overCount, r.reversed, r.minBlockH,
                r.lowBlocks);
    }

    // 附加行高（列表两行文字）对布局的影响：关闭 vs 开启的最小高度后置
    wprintf(L"\n-- 附加行高（rowExtraPx）对矩形高度的影响 --\n");
    for (int extra : { 0, 16 }) {
        AppSettings s = mk(10, 1, 2000);
        s.rowExtraPx = extra;
        std::vector<TreemapItem> out;
        ComputeTreemapRecursive(items, W, H, s, out);
        CheckResult r = Check(out, W, H);
        wprintf(L"rowExtraPx=%2d -> 矩形 %5zu  越界 %d  非父子重叠 %llu  覆盖 %.2f%%  最小块高 %dpx  <40px 块 %d\n",
                extra, out.size(), r.badBounds, r.overlapNonFamily, r.coverage, r.minBlockH,
                r.lowBlocks);
    }

    // SplitLongRects 单测（已移除：改用布局内行厚下限，形状问题只可能来自最小项）
    // 对数权重实证：最大/次大项 原始大小比 vs 布局权重比（log2(size+2)）
    {
        std::vector<ScanNode*> vs = items;
        std::stable_sort(vs.begin(), vs.end(),
                         [](const ScanNode* a, const ScanNode* b) { return a->size > b->size; });
        if (vs.size() >= 2) {
            double a0 = log2((double)vs[0]->size + 2.0);
            double a1 = log2((double)vs[1]->size + 2.0);
            double sizeRatio = (double)vs[0]->size / (double)std::max<ULONGLONG>(vs[1]->size, 1);
            wprintf(L"对数实证: 最大 %ls / 次大 %ls = %ls\n", vs[0]->name.c_str(),
                    vs[1]->name.c_str(), FormatSize(vs[0]->size).c_str());
            wprintf(L"          原始大小比 %.0f : 1, 布局权重比 %.2f : 1 (log2)\n", sizeRatio,
                    a0 / a1);
        }
    }

    // 渲染默认设置预览图
    std::vector<TreemapItem> out;
    AppSettings st;
    LoadSettings(st);
    ComputeTreemapRecursive(items, W, H, st, out);
    std::vector<DWORD> px((size_t)W * H, 0xFF1E2023);
    for (const auto& it : out) {
        COLORREF c = TreemapColor(it.node, st.colorScheme);
        DWORD bgr = RGB(GetBValue(c), GetGValue(c), GetRValue(c));
        for (int y = it.rc.top; y < it.rc.bottom && y < H; y++)
            for (int x = it.rc.left; x < it.rc.right && x < W; x++)
                px[(size_t)y * W + x] = bgr;
    }
    WriteBMP("treemap_preview.bmp", W, H, px);
    wprintf(L"预览图已写出: treemap_preview.bmp  (矩形 %zu)\n", out.size());

    // ---- TEMP DEBUG: dump strips ----
    struct Strip { std::wstring name; ULONGLONG sz; bool dir; int l,t,r,b; };
    std::vector<Strip> strips;
    for (const auto& it : out) {
        int w = it.rc.right - it.rc.left, h = it.rc.bottom - it.rc.top;
        double ar = (w >= h) ? (double)w / (double)h : (double)h / (double)w;
        if (ar > 4.0 || h < 6 || w < 6)
            strips.push_back({ it.node->name, it.node->size, it.node->isDir,
                               it.rc.left, it.rc.top, it.rc.right, it.rc.bottom });
    }
    std::sort(strips.begin(), strips.end(), [](const Strip& a, const Strip& b) {
        int wa = a.r - a.l, ha = a.b - a.t, wb = b.r - b.l, hb = b.b - b.t;
        double ara = (wa >= ha) ? (double)wa / ha : (double)ha / wa;
        double arb = (wb >= hb) ? (double)wb / hb : (double)hb / wb;
        return ara > arb;
    });
    wprintf(L"--- STRIP DUMP (%zu) ---\n", strips.size());
    for (size_t i = 0; i < strips.size() && i < 60; i++) {
        const Strip& s = strips[i];
        int w = s.r - s.l, h = s.b - s.t;
        double ar = (w >= h) ? (double)w / h : (double)h / w;
        wprintf(L"  AR=%.0f %dx%d  %s%s  size=%llu\n", ar, w, h,
                s.dir ? L"[D]" : L"", s.name.c_str(), s.sz);
    }
    // 按父目录分组统计
    std::map<ScanNode*, int> byParent;
    for (const auto& it : out) {
        int w = it.rc.right - it.rc.left, h = it.rc.bottom - it.rc.top;
        double ar = (w >= h) ? (double)w / (double)h : (double)h / (double)w;
        if (ar > 4.0) byParent[it.node->parent]++;
    }
    wprintf(L"--- STRIPS BY PARENT DIR ---\n");
    std::vector<std::pair<ScanNode*, int>> pp(byParent.begin(), byParent.end());
    std::sort(pp.begin(), pp.end(), [](const std::pair<ScanNode*, int>& a,
                                       const std::pair<ScanNode*, int>& b) {
        return a.second > b.second;
    });
    for (auto& kv : pp) {
        wprintf(L"  %d strips in [%s] (size=%llu)\n", kv.second,
                kv.first ? kv.first->name.c_str() : L"(root)", kv.first ? kv.first->size : 0);
    }
    // ---- TEMP DEBUG end ----
    return 0;
}
