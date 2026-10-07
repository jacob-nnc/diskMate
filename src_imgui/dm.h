// ============================================================================
// DiskMate —— ImGui 前端公共头
//   唯一基准：web/index.html（WebView2 版）
//   常量 / 配色 / 文本格式 / 全局状态 全部按 HTML 的 CSS 变量与 JS 公式对齐
// ============================================================================
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "imgui.h"

#include "scanner.h"
#include "scan_engine.h"
#include "settings.h"

// ------------------------------------------------------------ 几何常量 ----
// 单位：**逻辑像素**（= HTML 的 CSS px）。ImGui 的坐标空间就是逻辑像素：
// 每帧把 io.DisplaySize 换算成 物理尺寸/UI缩放，并设 io.DisplayFramebufferScale = UI缩放，
// 后端把顶点按 DPI 放大到物理像素，同时 ImGui 1.92 会用它作字体的 RasterizerDensity
// → 字形按物理尺寸栅格化（不糊），而布局/字号全部保持逻辑像素（与 HTML 逐项一致）。
extern float g_uiScale;
// 工具栏实际高度：窗口窄到一行放不下时，尾部控件换到第二行（不会再有按钮被切掉）
extern float g_toolbarH;
// 自检用：是否设置了自动点击（--click / --click2）。树图用它打穿透诊断。
bool DbgAutoClick();

namespace dm {
constexpr float ROW_H      = 30.0f;   // --row-h（JS ROW_H）
constexpr float HEAD_H     = 26.0f;   // .head 高度
constexpr float ARROW      = 22.0f;   // --arrow（JS ARROW）
constexpr float INDENT     = 16.0f;   // JS INDENT（每层缩进）
constexpr float GAP        = 6.0f;    // JS GAP（列间距）
constexpr float CRUMB_H    = 24.0f;   // .crumbs 高度
constexpr float STATUS_H   = 24.0f;   // .status 高度
constexpr float SPLIT_W    = 5.0f;    // .splitter 宽度
constexpr float TOOLBAR_BG_H = 50.0f;  // .toolbar: padding 10 + 控件 30 + padding 10
constexpr float TOOLBAR_ROW_H = 46.0f; // 工具栏换到第二行时每行占的高度
constexpr float LAYOUT_SCALE = 4.0f;  // JS LAY_SCALE（树图 4× 超采样虚拟画布）
constexpr float NAME_MIN   = 120.0f;  // NAME_MIN
constexpr float SPLIT_MIN  = 0.15f;   // SPLIT_MIN
constexpr float SPLIT_MAX  = 0.85f;
constexpr double WHEEL_MS  = 500.0;   // withinWheelWindow(): 500ms
constexpr double FADE_MS   = 220.0;   // .22s cubic-bezier(0.16,1,0.3,1)
constexpr double HINT_MS   = 700.0;   // 设置“已保存”提示停留
}

// 物理 1px 细线（HTML 的 --hair = 1px / dpr）：层级线 / 焦点框 / 目录外框用
#define HAIR (1.0f / (g_uiScale > 0.0f ? g_uiScale : 1.0f))

// 把「逻辑坐标」吸附到物理像素边界。
// 为什么必须：1px 细线（HAIR）在 150% 下等于 1 个物理像素，但如果线落在半个
// 物理像素上，GPU 光栅化只覆盖一半 → 再叠加 alpha 就几乎看不见了
// （踩过：树图「第一层级目录外框」明明画了 1 万多个像素，屏幕上却看不出来）。
inline float SnapPx(float logical) {
    if (g_uiScale <= 0.0f) return roundf(logical);
    return roundf(logical * g_uiScale) / g_uiScale;
}

// ------------------------------------------------------------ 行模型 ----
struct FlatRow {
    ScanNode* node = nullptr;
    int  depth = 0;
    bool hasKids = false;
    bool expanded = false;
    bool isRoot = false;
    bool up = false;
};

// -------------------------------------------------------- 扩展名配色组 ----
struct ExtGroup {
    std::wstring key;                  // ui-prefs.json 里的键（img/vid/…，Web 版同款）
    std::wstring name;                 // 显示名（短键 → 中文；自定义键 = 键名）
    ImU32 color = IM_COL32(90, 100, 114, 255);
    std::vector<std::wstring> exts;    // 小写、不带点
};

// ------------------------------------------------- 共用偏好存储（多前端）----
// ui-prefs.json（Web 页面的界面偏好 blob）+ diskmate.ini（lastPath / mft）
namespace prefs {
void LoadUiPrefs();
void SaveUiPrefs();
std::wstring IniGet(const wchar_t* key, const std::wstring& def);
void IniSet(const wchar_t* key, const std::wstring& value);
std::wstring UiPrefsPath();
std::wstring IniPath();
}

// ---------------------------------------------------------------- 工具 ----
std::wstring Wide(const std::string& s);
std::string  Utf8(const std::wstring& s);
std::string  FmtSize(ULONGLONG bytes);            // HTML fmtSize
std::wstring FmtSizeW(ULONGLONG bytes);
std::string  FmtCount(ULONGLONG n);               // HTML fmtCount（千分位）
std::wstring TypeOfNode(const ScanNode* n);       // 带点后缀 / 文件夹 / 文件
std::string  TypeOfNodeU8(const ScanNode* n);
std::wstring FullPathOf(const ScanNode* n);
void         LogLine(const std::wstring& s);
ImU32        AlphaOf(ImU32 c, int a);             // 改 alpha
ImU32        BlendText(ImU32 bg, ImU32 fg, float a);
std::string  Ellipsize(const char* text, float maxW, ImFont* font, float fontSize);

// ------------------------------------------------------------ 主题色 ----
// 对应 web/index.html :root / :root[data-theme=light]
ImU32 ColBg();            // --bg
ImU32 ColPanel();         // --panel
ImU32 ColLine();          // --line
ImU32 ColText();          // --text
ImU32 ColDim();           // --dim
ImU32 ColAccent();        // --accent
ImU32 ColBtn();           // --btn
ImU32 ColBtnHover();      // --btn-hover
ImU32 ColTreeLine();      // --tree-line
ImU32 ColDanger();        // button.danger #c0392b
ImU32 ColTmGround();      // 树图底色（GL clearColor）
ImU32 ColTmDir();         // 目录基色
ImU32 ColTmFrame();       // 目录外框 rgba(190,196,206,.55)
ImVec4 V4(ImU32 c);

// ------------------------------------------------------------ 全局状态 ----
extern AppSettings g_st;
extern ScanEngine  g_engine;
extern std::unique_ptr<ScanResult> g_tree;
extern ScanNode*   g_current;
extern std::atomic<bool> g_scanning;
extern std::wstring g_st0, g_st1, g_st2;     // 状态栏三段
extern char         g_pathBuf[1024];
extern std::wstring g_scanRoot;              // 上次扫描的根路径

extern std::vector<FlatRow> g_rows;
extern std::unordered_set<ScanNode*> g_expanded;
extern ScanNode* g_selNode;
extern std::unordered_set<ScanNode*> g_selSet;
extern int  g_anchorRow;
extern std::vector<ScanNode*> g_navHist;
extern int  g_sortKey;      // 0=名称 1=大小 2=占比 3=类型 4=文件数
extern bool g_sortAsc;
extern float g_nameW;
extern float g_colW[4];
extern float g_splitFrac;
extern std::vector<ExtGroup> g_extMap;
extern ImU32 g_extFallback;
extern int   g_layoutStamp;   // 树/设置/主题/配色变化时 ++ → 树图重布局
extern bool  g_animActive;    // 进入目录/出图过渡动画
extern double g_animT0;
extern bool  g_settingsDirty;

// 扫描进度（状态栏进度条 + 速度）
extern std::atomic<ULONGLONG> g_progItems, g_progBytes, g_progSkipped;
extern std::atomic<bool> g_progDirty;
extern bool          g_progVisible;
extern std::wstring  g_progText;

// 设置模态
extern bool g_showSettings;
extern AppSettings g_stEdit;
extern std::wstring g_setHint;
extern double g_setHintAt;

// MFT 扫描模式（0=普通遍历 1=强制 MFT 2=自动；按钮显示 = 管理员 && 非 0）
extern int  g_mftMode;
extern bool g_wantMftMode;

// 文件操作
extern std::vector<std::wstring> g_clipPaths;
extern bool   g_clipCut;
extern bool   g_renameOpen;
extern char   g_renameBuf[512];
extern ScanNode* g_renameNode;
extern bool   g_newFolderOpen;
extern char   g_newFolderBuf[512];

// ------------------------------------------------------------ 函数 ----
std::vector<ScanNode*> SortedChildren(const std::vector<ScanNode*>& in);
void SortTreeRecursive(ScanNode* n);
void BuildRows();
void Enter(ScanNode* node, bool noHistory = false);
void GoBack();
void GoUp();
void LocateInList(ScanNode* node);
void ScrollRowToView(int idx);
ScanNode* FindNodeByPath(const std::wstring& path);

void ShellOpenPath(const std::wstring& path);
void ShellExplorePath(const std::wstring& path);
void ShellTerminalPath(const std::wstring& path);
void ShellPropertiesPath(const std::wstring& path);
bool ClipboardSetText(const std::wstring& s);
void ShellDeletePath(const std::wstring& path, bool permanent, bool confirm);
void ShellVerb(const std::wstring& verb, ScanNode* node);
void RefreshSubtree(ScanNode* dir);
std::vector<std::wstring> SelectionPaths();

void StartScan(const std::wstring& root);
void ApplyImGuiTheme();
ImU32 ExtColorOf(const std::wstring& name);

float AnimT();   // 进入目录/出图过渡动画进度（0..1，1 = 无动画）

// treemap_panel.cpp
void DrawTreemapPanel(const ImVec2& size);
void TreemapInvalidate();
void TreemapOnEnter();
bool TreemapWheelActive();
ScanNode* TreemapWheelNode();
void TreemapClearWheel();   // 导航/重扫时清掉滚轮框选（防止留下错误的暗框）
void TreemapOnClickSelect(ScanNode* n);   // 供右键菜单等使用（无）

// shell_menu.cpp
void ShowShellContextMenuAsync(std::vector<std::wstring> paths);
void ShowCustomContextMenu(const std::vector<std::wstring>& paths);

// settings_modal.cpp
void DrawSettingsModal();
void DrawInputDialogs();

// 字体
extern ImFont* g_fontUi;     // 13px（默认）
extern ImFont* g_fontSm;     // 12px（表头/面包屑/状态栏/树图大字）
extern ImFont* g_fontXs;     // 11px（进度文字/树图小字）
