#pragma once
#include <windows.h>

#include <functional>
#include <vector>

#include "scanner.h"
#include "settings.h"

// 一个树图矩形条目
struct TreemapItem {
    ScanNode* node;
    RECT rc;  // 像素坐标（已裁剪到画布内）
    int depth = 0;     // 布局层级（顶层=0，每展开一层 +1）
    int layoutId = 0;  // 布局实例 id（同一布局调用 = 同一 id，用于同层面积校验）
};

// 树图消息（发给拥有者窗口）
#define WM_APP_TREEMAP_NAV (WM_APP + 11)  // wParam/lParam = ScanNode*；目录下钻，文件打开
#define WM_APP_TREEMAP_MENU (WM_APP + 12)  // wParam = ScanNode*，lParam = MAKELPARAM(屏幕坐标)
#define WM_APP_TREEMAP_SELECT_PARENT (WM_APP + 13)  // 选择当前选中项的父级
#define WM_APP_TREEMAP_SELECT_NODE (WM_APP + 14)  // wParam = ScanNode*：选中指定节点
#define WM_APP_TREEMAP_LOCATE (WM_APP + 15)  // 树图→主窗口：wParam = ScanNode*，双向绑定定位到列表
#define WM_APP_TREEMAP_SELECT_PARENT_DIR (WM_APP + 16)  // wParam = ScanNode*（当前目录）：框选其父级区域

// 纯布局算法（squarified treemap，可按大小独立测试）：
// 把 items 布局到 width×height 画布，至多 maxItems 项，全部矩形恰好铺满。
// w 可选权重函数（nullptr 时用 log2(size+2)）；layoutId 标识本次布局实例。
void ComputeTreemap(const std::vector<ScanNode*>& items, int width, int height,
                    size_t maxItems, std::vector<TreemapItem>& out,
                    const std::function<double(const ScanNode*)>& w = {}, int layoutId = 0);

// 递归版：文件夹按设置（递归层级 / 最小尺寸 / 矩形数 / 每文件夹展开上限）展开为其子项，
// 小于阈值或超层级的项绘制为叶子矩形。纯函数，可独立测试。
void ComputeTreemapRecursive(const std::vector<ScanNode*>& items, int width, int height,
                             const AppSettings& st, std::vector<TreemapItem>& out);

// 条目配色：目录为蓝色；文件按 scheme 着色
// scheme 0=按扩展名哈希调色板，1=按类型分组（图片/视频/音频/文档/压缩/代码/其他），2=单色
COLORREF TreemapColor(const ScanNode* node, int scheme);

// Win32 窗口接口
void TreemapRegister(HINSTANCE hInst);
HWND TreemapCreate(HWND parent, HINSTANCE hInst);
void TreemapSetData(HWND hwnd, const std::vector<ScanNode*>& items, ScanNode* current);
void TreemapSetSettings(HWND hwnd, const AppSettings& st);

// 单块蒙版：把整图按该节点的全部矩形压暗，只保留该节点的矩形为高亮（色块级联动）。
// 传 nullptr / 节点不在当前布局中则恢复全亮。
void TreemapSetBlockMask(HWND hwnd, ScanNode* node);
