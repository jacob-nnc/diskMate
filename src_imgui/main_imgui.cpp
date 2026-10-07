// ============================================================================
// DiskMate —— ImGui 前端主程序（对照 web/index.html 逐项复刻）
//   · 单进程、无脚本引擎、无共享内存；D3D11 渲染；复用后端扫描/配置/提权
//   · 布局：工具栏 / 面包屑 / 左侧树形列表(自绘) / 右侧 Treemap / 底部状态栏
//   · 几何、配色、文案、数字格式全部按 HTML 的 CSS 变量与 JS 公式对齐
// ============================================================================
#include "dm.h"
#include "jsonlite.h"
#include "imgui_internal.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <d3d11.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shellapi.h>

#include <mutex>

using namespace dm;

// ============================================================== 工具 =====
std::wstring Wide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring r((size_t)n, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &r[0], n);
    return r;
}
std::string Utf8(const std::wstring& s) {
    if (s.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string r((size_t)n, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &r[0], n, nullptr, nullptr);
    return r;
}
// HTML fmtSize：<1KB 原始 B；KB/MB 一位小数；GB/TB 两位小数
static void FmtSizeBuf(wchar_t* buf, size_t n, ULONGLONG b) {
    const double KB = 1024.0, MB = KB * 1024, GB = MB * 1024, TB = GB * 1024;
    double v = (double)b;
    if (v < KB) swprintf(buf, n, L"%llu B", b);
    else if (v < MB) swprintf(buf, n, L"%.1f KB", v / KB);
    else if (v < GB) swprintf(buf, n, L"%.1f MB", v / MB);
    else if (v < TB) swprintf(buf, n, L"%.2f GB", v / GB);
    else swprintf(buf, n, L"%.2f TB", v / TB);
}
std::wstring FmtSizeW(ULONGLONG b) {
    wchar_t buf[48];
    FmtSizeBuf(buf, 48, b);
    return buf;
}
std::string FmtSize(ULONGLONG b) { return Utf8(FmtSizeW(b)); }

// HTML fmtCount：toLocaleString("zh-CN") → 千分位
std::string FmtCount(ULONGLONG n) {
    std::wstring s = std::to_wstring(n);
    std::wstring out;
    int c = 0;
    for (size_t i = s.size(); i-- > 0;) {
        out.push_back(s[i]);
        if (++c % 3 == 0 && i > 0) out.push_back(L',');
    }
    std::reverse(out.begin(), out.end());
    return Utf8(out);
}

// HTML typeOf：目录“文件夹”；文件取【带点】后缀；无后缀“文件”
std::wstring TypeOfNode(const ScanNode* n) {
    if (!n) return L"";
    if (n->isDir) return L"文件夹";
    size_t d = n->name.find_last_of(L'.');
    if (d == std::wstring::npos || d + 1 >= n->name.size()) return L"文件";
    std::wstring ext = n->name.substr(d);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    return ext;
}
std::string TypeOfNodeU8(const ScanNode* n) { return Utf8(TypeOfNode(n)); }
std::wstring FullPathOf(const ScanNode* n) { return NodeFullPath(n); }

void LogLine(const std::wstring& s) {
    static FILE* fp = nullptr;
    if (!fp) {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir = exe;
        size_t sl = dir.find_last_of(L"\\/");
        dir = (sl == std::wstring::npos) ? L"" : dir.substr(0, sl + 1);
        _wfopen_s(&fp, (dir + L"diskmate_imgui.log").c_str(), L"ab");
    }
    if (!fp) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char head[64];
    int hn = snprintf(head, sizeof(head), "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute,
                      st.wSecond, st.wMilliseconds);
    std::string u = Utf8(s);
    fwrite(head, 1, (size_t)hn, fp);
    fwrite(u.c_str(), 1, u.size(), fp);
    fwrite("\n", 1, 1, fp);
    fflush(fp);
}

ImU32 AlphaOf(ImU32 c, int a) {
    return (c & ~IM_COL32_A_MASK) | ((ImU32)(a < 0 ? 0 : (a > 255 ? 255 : a)) << IM_COL32_A_SHIFT);
}
ImU32 BlendText(ImU32 bg, ImU32 fg, float a) {
    float br = ((bg >> IM_COL32_R_SHIFT) & 255) / 255.0f;
    float bgc = ((bg >> IM_COL32_G_SHIFT) & 255) / 255.0f;
    float bb = ((bg >> IM_COL32_B_SHIFT) & 255) / 255.0f;
    float fr = ((fg >> IM_COL32_R_SHIFT) & 255) / 255.0f;
    float fg2 = ((fg >> IM_COL32_G_SHIFT) & 255) / 255.0f;
    float fb = ((fg >> IM_COL32_B_SHIFT) & 255) / 255.0f;
    auto M = [&](float b2, float f2) { return (int)((b2 * (1 - a) + f2 * a) * 255.0f + 0.5f); };
    return IM_COL32(M(br, fr), M(bgc, fg2), M(bb, fb), 255);
}

// HTML fitText：按测量宽度裁字符 + 省略号（不是 canvas 的横向压扁）
std::string Ellipsize(const char* text, float maxW, ImFont* font, float fontSize) {
    if (!text || !*text || maxW <= 0) return "";
    if (!font) font = ImGui::GetFont();
    ImVec2 full = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text);
    if (full.x <= maxW) return text;
    if (full.x <= 0) return "";
    const char* ell = "\xe2\x80\xa6";   // …
    ImVec2 ew = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, ell);
    if (ew.x > maxW) return "";
    size_t lo = 0, hi = strlen(text);
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        ImVec2 sz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text, text + mid);
        if (sz.x + ew.x <= maxW) lo = mid;
        else hi = mid - 1;
    }
    if (lo == 0) return ell;
    return std::string(text, lo) + ell;
}

// ============================================================ 主题色 =====
// web/index.html :root / :root[data-theme="light"]
static inline bool Light() { return g_st.theme == 1; }
ImU32 ColBg()        { return Light() ? IM_COL32(0xf8, 0xfa, 0xfc, 255) : IM_COL32(0x1e, 0x22, 0x28, 255); }
ImU32 ColPanel()     { return Light() ? IM_COL32(0xee, 0xf1, 0xf5, 255) : IM_COL32(0x26, 0x2b, 0x33, 255); }
ImU32 ColLine()      { return Light() ? IM_COL32(0xd6, 0xdc, 0xe4, 255) : IM_COL32(0x14, 0x17, 0x1c, 255); }
ImU32 ColText()      { return Light() ? IM_COL32(0x18, 0x1e, 0x26, 255) : IM_COL32(0xe2, 0xe8, 0xf0, 255); }
ImU32 ColDim()       { return Light() ? IM_COL32(0x64, 0x6e, 0x7c, 255) : IM_COL32(0x96, 0xa2, 0xb2, 255); }
ImU32 ColAccent()    { return Light() ? IM_COL32(0x25, 0x63, 0xeb, 255) : IM_COL32(0x2d, 0x69, 0xff, 255); }
ImU32 ColBtn()       { return Light() ? IM_COL32(0xe3, 0xe8, 0xef, 255) : IM_COL32(0x3a, 0x40, 0x48, 255); }
ImU32 ColBtnHover()  { return Light() ? IM_COL32(0xd3, 0xda, 0xe4, 255) : IM_COL32(0x47, 0x4e, 0x57, 255); }
ImU32 ColTreeLine()  { return Light() ? IM_COL32(0xb3, 0xbc, 0xc8, 230) : IM_COL32(0x4a, 0x52, 0x5e, 230); }
ImU32 ColDanger()    { return IM_COL32(0xc0, 0x39, 0x2b, 255); }
ImU32 ColTmGround()  { return Light() ? IM_COL32(0xe8, 0xec, 0xf1, 255) : IM_COL32(0x2b, 0x31, 0x3b, 255); }
ImU32 ColTmDir()     { return Light() ? IM_COL32(0xc8, 0xcf, 0xd8, 255) : IM_COL32(0x3a, 0x42, 0x50, 255); }
ImU32 ColTmFrame()   { return IM_COL32(190, 196, 206, 140); }
ImVec4 V4(ImU32 c)   { return ImGui::ColorConvertU32ToFloat4(c); }

// ============================================================ 全局状态 ====
AppSettings g_st;
ScanEngine  g_engine;
std::unique_ptr<ScanResult> g_tree;
ScanNode*   g_current = nullptr;
std::atomic<bool> g_scanning{ false };
std::wstring g_st0 = L"就绪", g_st1, g_st2;
char         g_pathBuf[1024] = "C:\\";
std::wstring g_scanRoot;

std::vector<FlatRow> g_rows;
std::unordered_set<ScanNode*> g_expanded;
ScanNode* g_selNode = nullptr;
std::unordered_set<ScanNode*> g_selSet;
int  g_anchorRow = -1;
std::vector<ScanNode*> g_navHist;
int  g_sortKey = 1;          // HTML 初始：sortKey="size", sortAsc=false
bool g_sortAsc = false;
float g_nameW = 340.0f;
float g_colW[4] = { 110.0f, 80.0f, 96.0f, 90.0f };
float g_splitFrac = 0.55f;
std::vector<ExtGroup> g_extMap;
ImU32 g_extFallback = IM_COL32(0x5a, 0x64, 0x72, 255);
int   g_layoutStamp = 1;
bool  g_animActive = false;
double g_animT0 = 0.0;
bool  g_settingsDirty = false;

std::atomic<ULONGLONG> g_progItems{ 0 }, g_progBytes{ 0 }, g_progSkipped{ 0 };
std::atomic<bool> g_progDirty{ false };
bool         g_progVisible = false;
std::wstring g_progText;

bool g_showSettings = false;
AppSettings g_stEdit;
std::wstring g_setHint;
double g_setHintAt = 0.0;

std::vector<std::wstring> g_clipPaths;
bool g_clipCut = false;
int  g_mftMode = 0;
bool g_wantMftMode = false;
bool g_renameOpen = false;
char g_renameBuf[512] = "";
ScanNode* g_renameNode = nullptr;
bool g_newFolderOpen = false;
char g_newFolderBuf[512] = "";

ImFont* g_fontUi = nullptr;
ImFont* g_fontSm = nullptr;
ImFont* g_fontXs = nullptr;
float   g_uiScale = 1.0f;   // 监视器 DPI / 96
float   g_toolbarH = TOOLBAR_BG_H;   // 工具栏实际高度：窄窗口换第二行时由 DrawToolbar 加高

// 列表可视高度 / 待处理滚动 / 面板位置（跨帧）
static float s_listViewH = 300.0f;
static float s_listViewW = 400.0f;
static float s_lastScrollY = 0.0f;
static float s_pendingScrollY = -1.0f;
// 自检用：强制鼠标位置 / 列表滚动 / 启动即开设置（--hoverpos=x,y --scroll=N --opensettings
//            --click=x,y 模拟单击 --wheel=N 模拟上滚 N 格 --key=F2 模拟按键）
static ImVec2 g_dbgMouse{ -1e9f, -1e9f };
static float  g_dbgScroll = -1.0f;
static ImVec2 g_dbgClick{ -1e9f, -1e9f };
static ImVec2 g_dbgClick2{ -1e9f, -1e9f };   // 第二次点击（第 60 帧）
static ImVec2 g_dbgDblClick{ -1e9f, -1e9f }; // 真双击（第 30/34 帧）
bool DbgAutoClick() { return g_dbgClick.x > -1e8f || g_dbgClick2.x > -1e8f || g_dbgDblClick.x > -1e8f; }
static int    g_dbgWheelTicks = 0;
static int    g_frameNo = 0;
static std::wstring g_dbgKey;
static HWND  s_hwnd = nullptr;

// 扫描结果投递（工作线程 → UI 线程）
static std::mutex s_pendMx;
static double s_progT0 = 0.0;
static ULONGLONG s_progI0 = 0, s_progB0 = 0;
static std::unique_ptr<ScanResult> s_pendTree;
static ScanMode s_pendMode = ScanMode::Walk;
static ScanStats s_pendStats{};
static std::atomic<bool> s_pendReady{ false };
static std::atomic<bool> s_pendFail{ false };
static std::wstring s_pendFailReason;

// 打字机动画曲线（cubic-bezier(0.16,1,0.3,1) ≈ easeOutExpo）
float AnimT() {
    if (!g_animActive) return 1.0f;
    double dt = ImGui::GetTime() * 1000.0 - g_animT0;
    if (dt >= FADE_MS) { g_animActive = false; return 1.0f; }
    double t = dt / FADE_MS;
    if (t < 0) t = 0;
    float e = (float)(1.0 - pow(2.0, -10.0 * t));
    return e;
}
static ImU32 AnimA(ImU32 c) {
    float t = AnimT();
    if (t >= 0.999f) return c;
    int a = (int)(((c >> IM_COL32_A_SHIFT) & 255) * t + 0.5f);
    return AlphaOf(c, a);
}

// ============================================================ 树 / 行 =====
// 后端（HTML 宿主）在传树前把子项排成【目录优先 + size 降序】，
// 前端 sizedItems 不再排序 —— 这里扫描完成后同样处理一次，保证布局一致。
void SortTreeRecursive(ScanNode* root) {
    if (!root) return;
    std::vector<ScanNode*> stack;
    stack.push_back(root);
    while (!stack.empty()) {
        ScanNode* n = stack.back();
        stack.pop_back();
        std::stable_sort(n->children.begin(), n->children.end(),
                         [](const ScanNode* a, const ScanNode* b) {
                             if (a->isDir != b->isDir) return a->isDir;
                             return a->size > b->size;
                         });
        for (ScanNode* c : n->children) stack.push_back(c);
    }
}

// HTML sortItems()
std::vector<ScanNode*> SortedChildren(const std::vector<ScanNode*>& in) {
    std::vector<ScanNode*> out = in;
    std::stable_sort(out.begin(), out.end(), [](const ScanNode* a, const ScanNode* b) {
        int r = 0;
        switch (g_sortKey) {
            case 0: r = _wcsicmp(a->name.c_str(), b->name.c_str()); break;
            case 1: case 2: r = (a->size > b->size) ? 1 : (a->size < b->size ? -1 : 0); break;
            case 4: r = (a->fileCount > b->fileCount) ? 1 : (a->fileCount < b->fileCount ? -1 : 0); break;
            default: r = _wcsicmp(TypeOfNode(a).c_str(), TypeOfNode(b).c_str()); break;
        }
        return g_sortAsc ? (r < 0) : (r > 0);
    });
    return out;
}

// HTML buildRows()
void BuildRows() {
    g_rows.clear();
    ScanNode* root = g_tree ? g_tree->root : nullptr;
    bool rootOpen = root ? (g_expanded.count(root) > 0) : false;
    if (root) g_rows.push_back({ root, 0, true, rootOpen, true, false });
    int baseDepth = root ? 1 : 0;
    if (g_current && g_current->parent)
        g_rows.push_back({ g_current->parent, baseDepth, false, false, false, true });

    std::function<void(ScanNode*, int)> walk = [&](ScanNode* node, int depth) {
        bool hasKids = node->isDir && !node->children.empty();
        bool open = g_expanded.count(node) > 0;
        g_rows.push_back({ node, depth, hasKids, open, false, false });
        if (hasKids && open) {
            for (ScanNode* c : SortedChildren(node->children)) walk(c, depth + 1);
        }
    };
    std::vector<ScanNode*> view = g_current ? g_current->children : std::vector<ScanNode*>();
    if (root) {
        if (rootOpen) for (ScanNode* n : SortedChildren(view)) walk(n, baseDepth);
    } else {
        for (ScanNode* n : SortedChildren(view)) walk(n, 0);
    }
    // 状态栏 st1 / st2（HTML render() 尾部）
    if (g_current) {
        g_st1 = L"条目 " + Wide(FmtCount((ULONGLONG)g_rows.size()));
        g_st2 = L"总大小 " + FmtSizeW(g_current->size);
    } else {
        g_st1.clear();
        g_st2.clear();
    }
}

// HTML moreAfter()
static bool MoreAfter(int idx, int levelDepth) {
    for (int k = idx + 1; k < (int)g_rows.size(); k++) {
        int d = g_rows[k].depth;
        if (d < levelDepth) return false;
        if (d == levelDepth) return true;
    }
    return false;
}

// ---------------------------------------------------------------- 导航 ----
void Enter(ScanNode* node, bool noHistory) {
    if (!node || !node->isDir) return;
    if (!noHistory && g_current && g_current != node) {
        g_navHist.push_back(g_current);
        if (g_navHist.size() > 200) g_navHist.erase(g_navHist.begin());
    }
    g_current = node;
    // 路径框跟随当前目录：进子目录 / 双击树图块之后点「扫描」，扫的就是这个子目录。
    // （用户报过：进入子目录后扫描总是扫成父路径 —— 因为路径框还停在原来扫描的根）
    // 不做 WantTextInput 判断：导航是明确动作，那一帧 ImGui 还在用上一帧的 active id，
    // 判断会漏掉「先点路径框、再双击目录」这种顺序，反而更容易出问题。
    if (ImGui::GetCurrentContext()) {
        std::wstring fp = FullPathOf(node);
        if (!fp.empty()) {
            strcpy_s(g_pathBuf, Utf8(fp).c_str());
            g_st.lastPath = fp;
        }
        LogLine(L"ENTER " + fp + L" pathBuf=" + Wide(std::string(g_pathBuf)));
    }
    g_expanded.clear();
    if (g_tree && g_tree->root) g_expanded.insert(g_tree->root);
    g_selNode = nullptr;
    // HTML enter() 不清多选集合，但清掉属于别处的选中会让状态更直观；这里对齐 HTML：不清
    g_anchorRow = -1;
    BuildRows();
    TreemapClearWheel();   // HTML enter(): wheelStack.length=0 + 清焦点框
    TreemapInvalidate();
    TreemapOnEnter();
    s_pendingScrollY = 0.0f;
}
void GoBack() {
    if (!g_navHist.empty()) {
        ScanNode* p = g_navHist.back();
        g_navHist.pop_back();
        Enter(p, true);
    }
}
void GoUp() {
    if (g_current && g_current->parent) Enter(g_current->parent);
}

// HTML locateInList() + scrollToNode()
void LocateInList(ScanNode* node) {
    if (!node || !g_current) return;
    ScanNode* p = node->parent;
    int guard = 0;
    while (p && p != g_current && guard++ < 128) {
        if (p->isDir) g_expanded.insert(p);
        p = p->parent;
    }
    BuildRows();
    int idx = -1;
    for (size_t i = 0; i < g_rows.size(); i++)
        if (g_rows[i].node == node) { idx = (int)i; break; }
    if (idx >= 0) s_pendingScrollY = std::max(0.0f, (float)idx * ROW_H - floorf(s_listViewH / 3.0f));
}
void ScrollRowToView(int idx) {
    if (idx < 0) return;
    float top = (float)idx * ROW_H, bot = top + ROW_H;
    float y = s_lastScrollY;
    if (top < y) y = top;
    else if (bot > y + s_listViewH) y = bot - s_listViewH;
    s_pendingScrollY = std::max(0.0f, y);
}

// 按路径在已扫描的树里定位节点（右键菜单“进入此目录/定位”用）
ScanNode* FindNodeByPath(const std::wstring& path) {
    if (!g_tree || !g_tree->root || path.empty()) return nullptr;
    ScanNode* root = g_tree->root;
    std::wstring rp = FullPathOf(root);
    if (rp.empty() || path.size() < rp.size()) return nullptr;
    if (_wcsnicmp(path.c_str(), rp.c_str(), rp.size()) != 0) return nullptr;
    ScanNode* cur = root;
    size_t i = rp.size();
    while (i < path.size()) {
        while (i < path.size() && (path[i] == L'\\' || path[i] == L'/')) i++;
        if (i >= path.size()) break;
        size_t j = i;
        while (j < path.size() && path[j] != L'\\' && path[j] != L'/') j++;
        std::wstring seg = path.substr(i, j - i);
        ScanNode* next = nullptr;
        for (ScanNode* c : cur->children)
            if (_wcsicmp(c->name.c_str(), seg.c_str()) == 0) { next = c; break; }
        if (!next) return nullptr;
        cur = next;
        i = j;
    }
    return cur;
}

// ============================================================ Shell =====
bool ClipboardSetText(const std::wstring& s) {
    if (!OpenClipboard(s_hwnd)) return false;
    EmptyClipboard();
    size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hg) { CloseClipboard(); return false; }
    memcpy(GlobalLock(hg), s.c_str(), bytes);
    GlobalUnlock(hg);
    SetClipboardData(CF_UNICODETEXT, hg);
    CloseClipboard();
    return true;
}
// CF_HDROP：让 Ctrl+C 的文件列表与资源管理器互通
static void ClipboardSetFiles(const std::vector<std::wstring>& paths, bool cut) {
    if (paths.empty()) return;
    size_t bytes = sizeof(DROPFILES);
    for (const auto& p : paths) bytes += (p.size() + 1) * sizeof(wchar_t);
    bytes += sizeof(wchar_t);
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!hg) return;
    BYTE* base = (BYTE*)GlobalLock(hg);
    if (!base) { GlobalFree(hg); return; }
    DROPFILES* df = (DROPFILES*)base;
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    wchar_t* dst = (wchar_t*)(base + sizeof(DROPFILES));
    for (const auto& p : paths) {
        memcpy(dst, p.c_str(), p.size() * sizeof(wchar_t));
        dst += p.size();
        *dst++ = 0;
    }
    *dst = 0;
    GlobalUnlock(hg);
    if (!OpenClipboard(s_hwnd)) { GlobalFree(hg); return; }
    EmptyClipboard();
    SetClipboardData(CF_HDROP, hg);
    // 剪切标记（Preferred DropEffect = MOVE）
    if (cut) {
        UINT fmt = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
        HGLOBAL he = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
        if (he) {
            DWORD* d = (DWORD*)GlobalLock(he);
            *d = DROPEFFECT_MOVE;
            GlobalUnlock(he);
            SetClipboardData(fmt, he);
        }
    }
    CloseClipboard();
}
static std::vector<std::wstring> ClipboardGetFiles() {
    std::vector<std::wstring> out;
    if (!IsClipboardFormatAvailable(CF_HDROP)) return out;
    if (!OpenClipboard(s_hwnd)) return out;
    HANDLE h = GetClipboardData(CF_HDROP);
    if (h) {
        HDROP drop = (HDROP)h;
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; i++) {
            UINT len = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring p(len + 1, L'\0');
            DragQueryFileW(drop, i, &p[0], len + 1);
            p.resize(len);
            if (!p.empty()) out.push_back(p);
        }
    }
    CloseClipboard();
    return out;
}

void ShellOpenPath(const std::wstring& path) {
    if (path.empty()) return;
    ShellExecuteW(s_hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    LogLine(L"open " + path);
}
void ShellExplorePath(const std::wstring& path) {
    if (path.empty()) return;
    std::wstring arg = L"/select,\"" + path + L"\"";
    ShellExecuteW(s_hwnd, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
}
void ShellTerminalPath(const std::wstring& path) {
    std::wstring dir = path;
    DWORD a = GetFileAttributesW(path.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) {
        size_t s = path.find_last_of(L"\\/");
        if (s != std::wstring::npos) dir = path.substr(0, s);
    }
    std::wstring args = L"/K cd /d \"" + dir + L"\"";
    ShellExecuteW(s_hwnd, L"open", L"cmd.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}
void ShellPropertiesPath(const std::wstring& path) {
    if (path.empty()) return;
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_INVOKEIDLIST;
    sei.lpVerb = L"properties";
    sei.lpFile = path.c_str();
    sei.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&sei);
}

// FO_DELETE（回收站 / 永久）；confirm 时走系统确认框
void ShellDeletePath(const std::wstring& path, bool permanent, bool confirm) {
    if (path.empty() || path.size() >= MAX_PATH - 2) return;
    std::vector<wchar_t> from(path.size() + 2, 0);
    wcscpy_s(from.data(), from.size(), path.c_str());
    SHFILEOPSTRUCTW op{};
    op.hwnd = s_hwnd;
    op.wFunc = FO_DELETE;
    op.pFrom = from.data();
    op.fFlags = FOF_ALLOWUNDO;
    if (permanent) op.fFlags = 0;
    if (!confirm) op.fFlags |= FOF_NOCONFIRMATION;
    op.fFlags |= FOF_NOERRORUI;
    int r = SHFileOperationW(&op);
    LogLine(L"delete " + path + L" rc=" + std::to_wstring(r));
    if (r == 0) {
        // 本地更新模型：从父节点摘掉并向上重算聚合
        ScanNode* n = nullptr;
        for (ScanNode* c : (g_current ? g_current->children : std::vector<ScanNode*>()))
            if (FullPathOf(c) == path) { n = c; break; }
        if (n && n->parent) {
            ScanNode* par = n->parent;
            auto& kids = par->children;
            kids.erase(std::remove(kids.begin(), kids.end(), n), kids.end());
            for (ScanNode* up = par; up; up = up->parent) {
                ULONGLONG sz = 0, fc = 0;
                for (ScanNode* c : up->children) { sz += c->size; fc += c->fileCount; }
                up->size = sz;
                up->fileCount = fc;
            }
            g_selSet.erase(n);
            if (g_selNode == n) g_selNode = nullptr;
            g_expanded.erase(n);
            g_layoutStamp++;
            BuildRows();
        }
    }
}

// HTML cmd:"shell"（重命名 / 新建文件夹 / 复制 / 剪切 / 粘贴 / 删除）
void ShellVerb(const std::wstring& verb, ScanNode* node) {
    std::wstring path = node ? FullPathOf(node) : L"";
    if (verb == L"rename") {
        if (!node) return;
        g_renameNode = node;
        strcpy_s(g_renameBuf, Utf8(node->name).c_str());
        g_renameOpen = true;
    } else if (verb == L"newfolder") {
        g_newFolderOpen = true;
        g_newFolderBuf[0] = 0;
    } else if (verb == L"copy" || verb == L"cut") {
        std::vector<std::wstring> sel = SelectionPaths();
        if (sel.empty() && node) sel.push_back(path);
        if (sel.empty()) return;
        g_clipPaths = sel;
        g_clipCut = (verb == L"cut");
        ClipboardSetFiles(sel, g_clipCut);
        g_st0 = (g_clipCut ? L"已剪切 " : L"已复制 ") + std::to_wstring(sel.size()) + L" 项";
    } else if (verb == L"paste") {
        if (!g_current) return;
        std::vector<std::wstring> src = ClipboardGetFiles();
        if (src.empty()) src = g_clipPaths;
        if (src.empty()) { g_st0 = L"剪贴板里没有文件"; return; }
        std::wstring dstDir = FullPathOf(g_current);
        if (dstDir.empty()) return;
        std::wstring from;
        for (const auto& p : src) { from += p; from.push_back(L'\0'); }
        from.push_back(L'\0');
        std::wstring to = dstDir;
        to.push_back(L'\0');
        to.push_back(L'\0');
        SHFILEOPSTRUCTW op{};
        op.hwnd = s_hwnd;
        op.wFunc = g_clipCut ? FO_MOVE : FO_COPY;
        op.pFrom = from.c_str();
        op.pTo = to.c_str();
        op.fFlags = FOF_ALLOWUNDO;
        int r = SHFileOperationW(&op);
        LogLine(L"paste rc=" + std::to_wstring(r) + L" n=" + std::to_wstring(src.size()));
        if (r == 0) {
            if (g_clipCut) { g_clipPaths.clear(); g_clipCut = false; }
            g_st0 = L"已粘贴 " + std::to_wstring(src.size()) + L" 项，正在刷新…";
            RefreshSubtree(g_current);
        }
    } else if (verb == L"delete" || verb == L"delete_perm") {
        std::vector<std::wstring> sel = SelectionPaths();
        if (sel.empty() && node) sel.push_back(path);
        for (const auto& p : sel)
            ShellDeletePath(p, verb == L"delete_perm", g_st.confirmDelete);
    }
}

std::vector<std::wstring> SelectionPaths() {
    std::vector<std::wstring> out;
    for (const auto& r : g_rows) {
        if (!g_selSet.count(r.node)) continue;
        std::wstring p = FullPathOf(r.node);
        if (!p.empty()) out.push_back(p);
    }
    return out;
}

// 文件操作后刷新某个目录的子树（重扫该目录，把 arena 并入当前树）
void RefreshSubtree(ScanNode* dir) {
    if (!dir || !dir->isDir || !g_tree) return;
    std::wstring path = FullPathOf(dir);
    if (path.empty()) return;
    std::atomic<bool> cancel{ false };
    ScanStats st{};
    ScanResult sub = ScanTree(path, cancel, &st, nullptr, g_st.threads, g_st.skipHidden,
                              g_st.followReparse);
    if (!sub.root) return;
    sub.root->name = dir->name;
    ScanNode* newRoot = sub.root;
    std::unordered_set<ScanNode*> oldSet;
    {
        std::vector<ScanNode*> stack{ dir };
        while (!stack.empty()) {
            ScanNode* n = stack.back();
            stack.pop_back();
            oldSet.insert(n);
            for (ScanNode* c : n->children) stack.push_back(c);
        }
    }
    ScanNode* par = dir->parent;
    newRoot->parent = par;
    if (par) {
        for (auto& c : par->children) if (c == dir) c = newRoot;
    } else {
        g_tree->root = newRoot;
    }
    for (auto& up : sub.arena) g_tree->arena.push_back(std::move(up));
    sub.arena.clear();
    for (ScanNode* n : oldSet) { g_expanded.erase(n); g_selSet.erase(n); }
    if (g_selNode && oldSet.count(g_selNode)) g_selNode = nullptr;
    if (g_current && oldSet.count(g_current)) g_current = newRoot;
    for (ScanNode* up = par; up; up = up->parent) {
        ULONGLONG sz = 0, fc = 0;
        for (ScanNode* c : up->children) { sz += c->size; fc += c->fileCount; }
        up->size = sz;
        up->fileCount = fc;
    }
    if (g_current) g_expanded.insert(g_current == newRoot ? newRoot : g_current);
    g_layoutStamp++;
    BuildRows();
    g_st0 = L"已刷新 " + dir->name;
    LogLine(L"RefreshSubtree " + path);
}

// ------------------------------------------------------------ 扩展名色 ----
ImU32 ExtColorOf(const std::wstring& name) {
    size_t d = name.find_last_of(L'.');
    if (d == std::wstring::npos || d + 1 >= name.size()) return g_extFallback;
    std::wstring ext = name.substr(d + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    for (const auto& grp : g_extMap)
        for (const auto& e : grp.exts)
            if (e == ext) return grp.color;
    return g_extFallback;
}

// ------------------------------------------------------------ 扫描 ----
static void ApplyScanResult() {
    std::unique_ptr<ScanResult> tree;
    ScanMode mode;
    ScanStats stats;
    {
        std::lock_guard<std::mutex> lk(s_pendMx);
        tree = std::move(s_pendTree);
        mode = s_pendMode;
        stats = s_pendStats;
    }
    g_tree = std::move(tree);
    g_scanning = false;
    g_progVisible = false;
    g_expanded.clear();
    g_navHist.clear();
    g_selNode = nullptr;
    g_selSet.clear();
    g_anchorRow = -1;
    g_current = g_tree ? g_tree->root : nullptr;
    g_clipPaths.clear(); g_clipCut = false;
    if (g_tree && g_tree->root) {
        SortTreeRecursive(g_tree->root);
        g_expanded.insert(g_tree->root);
    }
    g_layoutStamp++;
    BuildRows();
    TreemapClearWheel();   // 新树：旧的滚轮框选坐标全部失效
    TreemapInvalidate();
    if (g_current) {
        g_st0 = L"扫描完成 · " + std::wstring(mode == ScanMode::Mft ? L"MFT 直读" : L"普通遍历") +
                L" · " + FullPathOf(g_current) + L" · " + Wide(FmtCount(stats.items)) + L" 项";
        TreemapOnEnter();
    } else {
        g_st0 = L"扫描失败（root 为空）";
    }
    LogLine(L"SCAN DONE mode=" + std::wstring(mode == ScanMode::Mft ? L"MFT" : L"Walk") +
            L" items=" + std::to_wstring(stats.items) + L" bytes=" + std::to_wstring(stats.bytes));
}

void StartScan(const std::wstring& rootArg) {
    std::wstring root = rootArg;
    if (root.empty()) root = L"C:\\";
    if (g_scanning) { g_engine.Cancel(); return; }
    // 路径无效：状态栏提示并复位（HTML 宿主 scanfail）
    DWORD attr = GetFileAttributesW(root.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        g_st0 = L"路径无效：" + root;
        g_scanning = false;
        g_progVisible = false;
        LogLine(L"scan rejected (invalid path): " + root);
        return;
    }
    g_scanRoot = root;
    g_scanning = true;
    g_progItems = 0; g_progBytes = 0; g_progSkipped = 0;
    g_progDirty = true;
    g_progVisible = true;
    g_progText = L"开始扫描…";
    s_progT0 = 0.0; s_progI0 = 0; s_progB0 = 0;
    g_st0 = L"扫描中…";
    strcpy_s(g_pathBuf, Utf8(root).c_str());
    g_st.lastPath = root;
    SaveSettings(g_st);
    prefs::IniSet(L"lastPath", root);   // 共用 diskmate.ini（Web 宿主同款键）
    LogLine(L"StartScan root=" + root + L" mode=" +
            (g_wantMftMode ? L"MFT" : L"Walk"));
    g_engine.SetCallbacks(
        [](ULONGLONG items, ULONGLONG bytes, ULONGLONG skipped) {
            g_progItems = items; g_progBytes = bytes; g_progSkipped = skipped;
            g_progDirty = true;
        },
        [](ScanDoneInfo&& info) {
            std::lock_guard<std::mutex> lk(s_pendMx);
            s_pendTree = std::move(info.tree);
            s_pendMode = info.mode;
            s_pendStats = info.stats;
            s_pendReady = true;
        },
        [](const std::wstring& reason) {
            std::lock_guard<std::mutex> lk(s_pendMx);
            s_pendFailReason = reason;
            s_pendFail = true;
        });
    ScanMode want = g_wantMftMode ? ScanMode::Mft : ScanMode::Walk;
    if (!g_engine.Start(root, want, g_st)) {
        g_scanning = false;
        g_progVisible = false;
        g_st0 = L"扫描未开始";
    }
}

// ====================================================== 扩展名配色默认值 ====
// ui-prefs.json 里没有 extMap 时的出厂表（与 web/index.html 的 extMap 完全一致）
static void InitDefaultExtMap() {
    g_extMap.clear();
    auto add = [](const wchar_t* key, const wchar_t* name, ImU32 color,
                  std::initializer_list<const wchar_t*> exts) {
        ExtGroup g;
        g.key = key;
        g.name = name;
        g.color = color;
        for (const wchar_t* e : exts) g.exts.push_back(e);
        g_extMap.push_back(std::move(g));
    };
    add(L"img", L"图片", IM_COL32(0xd9, 0x8a, 0x4f, 255),
        { L"jpg", L"jpeg", L"png", L"gif", L"bmp", L"webp", L"svg", L"ico", L"tif", L"tiff",
          L"heic", L"raw" });
    add(L"vid", L"视频", IM_COL32(0x4f, 0x9f, 0xd9, 255),
        { L"mp4", L"mkv", L"avi", L"mov", L"wmv", L"flv", L"webm", L"m4v", L"mpg", L"mpeg", L"ts",
          L"rmvb" });
    add(L"aud", L"音频", IM_COL32(0xa6, 0x6f, 0xd9, 255),
        { L"mp3", L"wav", L"flac", L"aac", L"ogg", L"m4a", L"wma", L"ape" });
    add(L"doc", L"文档", IM_COL32(0xd9, 0xb8, 0x4f, 255),
        { L"doc", L"docx", L"pdf", L"txt", L"md", L"xls", L"xlsx", L"ppt", L"pptx", L"csv", L"odt",
          L"rtf" });
    add(L"zip", L"压缩", IM_COL32(0xd9, 0x5f, 0x5f, 255),
        { L"zip", L"rar", L"7z", L"tar", L"gz", L"bz2", L"xz", L"z" });
    add(L"code", L"代码", IM_COL32(0x4f, 0xbf, 0xa6, 255),
        { L"c", L"cpp", L"h", L"hpp", L"py", L"js", L"ts", L"java", L"cs", L"go", L"rs", L"json",
          L"xml", L"html", L"css", L"sh", L"bat", L"sql" });
    add(L"exe", L"可执行", IM_COL32(0x5f, 0xbf, 0x6f, 255),
        { L"exe", L"msi", L"apk", L"appx", L"msix" });
    add(L"lib", L"库/系统", IM_COL32(0x6f, 0x8f, 0xd9, 255),
        { L"dll", L"sys", L"drv", L"ocx", L"dat" });
    add(L"vdisk", L"虚拟磁盘", IM_COL32(0x8f, 0xbf, 0x5f, 255),
        { L"vdi", L"vhd", L"vhdx", L"vmdk", L"qcow", L"iso", L"nrg", L"bin" });
    add(L"log", L"日志/配置", IM_COL32(0x5a, 0xb8, 0xc9, 255),
        { L"log", L"ini", L"cfg", L"conf", L"cache", L"tmp" });
}

// ============================================================ 字体/主题 ====
static void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    // 字号按【逻辑像素】给（= HTML 的 13 CSS px）。
    // ImGui 1.92 的字体系统会拿 io.DisplayFramebufferScale 当 RasterizerDensity，
    // 自动按物理尺寸栅格化字形 —— 不需要手动放大字号，也不会糊。
    static const ImWchar* ranges = io.Fonts->GetGlyphRangesChineseFull();
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    cfg.PixelSnapH = true;
    g_fontUi = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", 13.0f, &cfg, ranges);
    if (!g_fontUi) g_fontUi = io.Fonts->AddFontDefault();
    // 合并 Segoe UI Symbol：雅黑缺 ⚙(U+2699) ✕(U+2715) ▸(U+25B8) ▾(U+25BE) 等符号，
    // 浏览器会自动字体回退，ImGui 必须显式合并（否则画成 '?'）
    {
        static const ImWchar symRanges[] = {
            0x00B7, 0x00B7,
            0x2000, 0x206F,   // 常用标点 … ‹ ›
            0x2190, 0x21FF,   // 箭头 ← ↑ →
            0x25A0, 0x25FF,   // 几何形状 ▸ ▾ ● ■
            0x2699, 0x2699,   // ⚙
            0x2715, 0x2715,   // ✕
            0,
        };
        ImFontConfig sym;
        sym.MergeMode = true;
        sym.PixelSnapH = true;
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisym.ttf", 13.0f, &sym, symRanges);
    }
    g_fontSm = g_fontUi;
    g_fontXs = g_fontUi;
    io.FontDefault = g_fontUi;
    io.Fonts->Build();
    ImGui_ImplDX11_InvalidateDeviceObjects();
}

void ApplyImGuiTheme() {
    ImGuiStyle& st = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg] = V4(ColBg());
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = V4(ColPanel());
    c[ImGuiCol_Text] = V4(ColText());
    c[ImGuiCol_TextDisabled] = V4(ColDim());
    c[ImGuiCol_Border] = V4(ColLine());
    c[ImGuiCol_FrameBg] = V4(ColBg());
    c[ImGuiCol_FrameBgHovered] = V4(ColBtnHover());
    c[ImGuiCol_FrameBgActive] = V4(ColBtnHover());
    c[ImGuiCol_Button] = V4(ColBtn());
    c[ImGuiCol_ButtonHovered] = V4(ColBtnHover());
    c[ImGuiCol_ButtonActive] = V4(ColBtn());
    c[ImGuiCol_Header] = V4(ColBtnHover());
    c[ImGuiCol_HeaderHovered] = V4(ColBtnHover());
    c[ImGuiCol_HeaderActive] = V4(ColBtnHover());
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = V4(ColDim());
    c[ImGuiCol_ScrollbarGrabHovered] = V4(ColText());
    c[ImGuiCol_ScrollbarGrabActive] = V4(ColText());
    c[ImGuiCol_CheckMark] = V4(ColAccent());
    c[ImGuiCol_SliderGrab] = V4(ColAccent());
    c[ImGuiCol_SliderGrabActive] = V4(ColAccent());
    c[ImGuiCol_Separator] = V4(ColLine());
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.5f);

    st.WindowPadding = ImVec2(0, 0);
    st.FramePadding = ImVec2(8, 4);
    st.ItemSpacing = ImVec2(8, 4);
    st.ItemInnerSpacing = ImVec2(4, 4);
    st.WindowRounding = 0.0f;
    st.ChildRounding = 0.0f;
    st.FrameRounding = 6.0f;
    st.PopupRounding = 6.0f;
    st.ScrollbarSize = 12.0f;
    st.ScrollbarRounding = 0.0f;
    st.GrabMinSize = 8.0f;
    st.WindowBorderSize = 0.0f;
    st.ChildBorderSize = 0.0f;
    st.FrameBorderSize = 0.0f;
    st.PopupBorderSize = 1.0f;
}

// ============================================================ 列表 =======
// 列几何（HTML layoutCols）：NAME_X = ARROW；其余列按累积宽度
struct ColGeom { float nameX, nameW; float x[4], w[4]; };
static ColGeom ColsLayout() {
    ColGeom g;
    g.nameX = ARROW;
    g.nameW = std::max(NAME_MIN, g_nameW);
    float x = ARROW + g.nameW;
    for (int i = 0; i < 4; i++) {
        g.x[i] = x + GAP;
        g.w[i] = g_colW[i];
        x += g_colW[i] + GAP;
    }
    return g;
}

static void DrawListHeader(ImDrawList* dl, const ImVec2& p, float w, const ColGeom& cg) {
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + HEAD_H), ColPanel());
    dl->AddLine(ImVec2(p.x, p.y + HEAD_H), ImVec2(p.x + w, p.y + HEAD_H), ColLine());
    ImFont* f = g_fontSm ? g_fontSm : ImGui::GetFont();
    auto label = [&](const char* t, float x) {
        dl->AddText(f, 12.0f, ImVec2(p.x + x + 6, p.y + (HEAD_H - 12.0f) / 2.0f - 1), ColDim(), t);
    };
    label("名称", cg.nameX);
    static const char* names[4] = { "大小", "占比", "类型", "文件数" };
    for (int i = 0; i < 4; i++) label(names[i], cg.x[i]);
}

// 树形连接线（HTML treeLines / moreAfter + .tl CSS）
static void DrawTreeLines(ImDrawList* dl, const ImVec2& p, const FlatRow& r, int idx, float y,
                          float rowH) {
    if (r.depth <= 0) return;
    ImU32 col = ColTreeLine();
    const float hair = HAIR;   // HTML: --hair = 1px/dpr（物理 1px）
    float yc = y + rowH * 0.5f;
    for (int d = 0; d < r.depth; d++) {
        float lx = p.x + ARROW + (float)d * INDENT;
        bool isOwn = (d == r.depth - 1);
        int level = isOwn ? r.depth : d;
        bool more = MoreAfter(idx, level);
        if (isOwn) {
            float yEnd = more ? (y - 1 + rowH + 2) : yc;
            dl->AddRectFilled(ImVec2(lx, y - 1), ImVec2(lx + hair, yEnd), col);
            dl->AddRectFilled(ImVec2(lx, yc - 0.5f), ImVec2(lx + 9.0f, yc + 0.5f), col);
        } else if (more) {
            dl->AddRectFilled(ImVec2(lx, y - 1), ImVec2(lx + hair, y - 1 + rowH + 2), col);
        }
    }
}

// 单行绘制（HTML rowHtml）
static void DrawRow(ImDrawList* dl, const ImVec2& p, float w, const ColGeom& cg, const FlatRow& r,
                    int idx, float y, bool hovered) {
    ScanNode* node = r.node;
    ImU32 bg = ColBg();
    if (hovered) dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + w, y + ROW_H), AnimA(BlendText(bg, ColText(), 0.06f)));
    if (g_selSet.count(node))
        dl->AddRect(ImVec2(p.x + 0.5f, y + 0.5f), ImVec2(p.x + w - 0.5f, y + ROW_H - 0.5f),
                    AnimA(IM_COL32(255, 255, 255, 242)));

    ImFont* f = g_fontUi ? g_fontUi : ImGui::GetFont();
    const float ty = y + (ROW_H - 13.0f) / 2.0f - 1.0f;

    // 箭头列：固定 22px、居中（HTML .twist 是 ▸/▾ 字符，这里同款字形）
    float acx = p.x + ARROW * 0.5f, acy = y + ROW_H * 0.5f;
    if (r.hasKids) {
        const char* glyph = r.expanded ? "\xe2\x96\xbe" : "\xe2\x96\xb8";   // ▾ / ▸
        ImFont* af = g_fontUi ? g_fontUi : ImGui::GetFont();
        ImVec2 gs = af->CalcTextSizeA(15.0f, FLT_MAX, 0.0f, glyph);
        dl->AddText(af, 15.0f, ImVec2(acx - gs.x * 0.5f, acy - gs.y * 0.5f - 1.0f),
                    AnimA(ColDim()), glyph);
    }

    DrawTreeLines(dl, p, r, idx, y, ROW_H);

    // 名称列：left = ARROW + depth*INDENT，width = nameW - depth*INDENT（clip + 省略号）
    float nx = p.x + cg.nameX + (float)r.depth * INDENT;
    float nw = cg.nameW - (float)r.depth * INDENT;
    std::string nm = r.up ? ".." : Utf8(node->name);
    if (nw > 4) {
        std::string vis = Ellipsize(nm.c_str(), nw - 1.0f, f, 13.0f);
        dl->AddText(f, 13.0f, ImVec2(nx, ty), AnimA(r.isRoot ? ColDim() : ColText()), vis.c_str());
    }

    // 数据列（HTML：行内无左内边距，文字紧贴列 x）
    auto cell = [&](int i, const std::string& s) {
        if (s.empty()) return;
        dl->AddText(f, 13.0f, ImVec2(p.x + cg.x[i], ty), AnimA(ColDim()), s.c_str());
    };
    if (!r.up) {
        double total = (g_current && g_current->size) ? (double)g_current->size : 0.0;
        char pct[32];
        snprintf(pct, sizeof(pct), "%.1f%%", total > 0 ? (double)node->size / total * 100.0 : 0.0);
        cell(0, FmtSize(node->size));
        cell(1, pct);
        cell(2, TypeOfNodeU8(node));
        cell(3, FmtCount(node->fileCount));
    }
}

// 吸顶行（HTML updatePinRow）
static int PinRowIndex() {
    int n = (int)g_rows.size();
    if (n <= 0) return -1;
    float scrollY = (s_pendingScrollY >= 0) ? s_pendingScrollY : ImGui::GetScrollY();
    int firstIdx = (int)floorf(scrollY / ROW_H);
    if (firstIdx >= n) firstIdx = n - 1;
    int pinned = -1;
    for (int i = std::min(firstIdx, n - 1); i >= 0; i--) {
        const FlatRow& r = g_rows[i];
        if (r.hasKids && r.expanded) {
            const FlatRow& first = g_rows[firstIdx];
            if (first.depth > r.depth) pinned = i;
            break;
        }
    }
    const FlatRow& top = g_rows[firstIdx];
    if (top.hasKids && top.expanded) pinned = -1;
    return pinned;
}

static void DrawListPanel(const ImVec2& size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetWindowPos();
    ColGeom cg = ColsLayout();
    s_listViewW = size.x;
    s_listViewH = size.y;

    int n = (int)g_rows.size();
    float contentH = (float)n * ROW_H;
    ImGui::Dummy(ImVec2(0, std::max(contentH, size.y)));
    if (s_pendingScrollY >= 0) {
        ImGui::SetScrollY(s_pendingScrollY);
        s_pendingScrollY = -1.0f;
    }
    float scrollY = ImGui::GetScrollY();
    float winH = ImGui::GetWindowHeight();
    s_lastScrollY = scrollY;

    int first = std::max(0, (int)floorf(scrollY / ROW_H) - 2);
    int visible = (int)ceilf(winH / ROW_H) + 4;
    int last = std::min(n, first + visible);

    // 吸顶行要先算出来：它盖在可视区第一行上，命中测试也要优先落在它身上
    int pinIdx = PinRowIndex();

    ImVec2 mouse = ImGui::GetIO().MousePos;
    bool mouseInList = mouse.x >= p.x && mouse.x < p.x + size.x && mouse.y >= p.y &&
                       mouse.y < p.y + size.y;
    // 正常情况用 ImGui 的窗口命中；窗口刚被点活（AppFocusLost）那一帧 ImGui 会清掉
    // HoveredWindow，会让「第一次点击」被吞掉 —— 这里用矩形兜底（有模态时仍拦住）
    bool winHover = ImGui::IsWindowHovered() ||
                    (mouseInList && ImGui::GetIO().AppFocusLost && !g_showSettings &&
                     !g_renameOpen && !g_newFolderOpen &&
                     !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
                                                  ImGuiPopupFlags_AnyPopupLevel));
    int hoverRow = -1;
    if (winHover && mouseInList) {
        if (pinIdx >= 0 && mouse.y < p.y + ROW_H) {
            hoverRow = pinIdx;   // 吸顶行（浮动父级目录）优先命中
        } else {
            int idx = (int)floorf((mouse.y - p.y + scrollY) / ROW_H);
            if (idx >= 0 && idx < n) hoverRow = idx;
        }
    }

    if (n == 0) {
        ImFont* f = g_fontUi ? g_fontUi : ImGui::GetFont();
        const char* empty = "还没有数据 — 输入路径后点「扫描」";
        ImVec2 sz = f->CalcTextSizeA(13.0f, FLT_MAX, 0.0f, empty);
        dl->AddText(f, 13.0f, ImVec2(p.x + (size.x - sz.x) * 0.5f, p.y + 40.0f), ColDim(), empty);
    }

    for (int i = first; i < last; i++) {
        float y = p.y + (float)i * ROW_H - scrollY;
        if (y + ROW_H < p.y || y > p.y + size.y) continue;
        DrawRow(dl, p, size.x, cg, g_rows[i], i, y, i == hoverRow);
    }

    // ---- 吸顶行 ----
    if (pinIdx >= 0) {
        float py = p.y;
        dl->AddRectFilled(ImVec2(p.x, py), ImVec2(p.x + size.x, py + ROW_H), ColPanel());
        dl->AddLine(ImVec2(p.x, py + ROW_H), ImVec2(p.x + size.x, py + ROW_H), ColLine());
        DrawRow(dl, p, size.x, cg, g_rows[pinIdx], pinIdx, py, hoverRow == pinIdx);
    }

    // ---- 交互：点击箭头 / 选行 / 双击 ----
    if (winHover && hoverRow >= 0) {
        const FlatRow& r = g_rows[hoverRow];
        float mx = mouse.x - p.x;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (g_dbgClick.x > -1e8f)   // 自检：核对「物理点击点 → 逻辑坐标 → 命中行」
                LogLine(L"LISTCLICK phys=" + std::to_wstring((int)g_dbgClick.x) + L"," +
                        std::to_wstring((int)g_dbgClick.y) + L" logical=" +
                        std::to_wstring((int)mouse.x) + L"," + std::to_wstring((int)mouse.y) +
                        L" row=" + std::to_wstring(hoverRow) + L" node=" +
                        (g_rows[hoverRow].node ? g_rows[hoverRow].node->name : L"?"));
            if (mx < ARROW && r.hasKids) {
                if (r.expanded) g_expanded.erase(r.node); else g_expanded.insert(r.node);
                BuildRows();
            } else {
                ImGuiIO& io = ImGui::GetIO();
                bool ctrl = io.KeyCtrl, shift = io.KeyShift;
                if (shift && g_anchorRow >= 0) {
                    int a = std::min(g_anchorRow, hoverRow), b = std::max(g_anchorRow, hoverRow);
                    if (!ctrl) g_selSet.clear();
                    for (int k = a; k <= b; k++) g_selSet.insert(g_rows[k].node);
                } else if (ctrl) {
                    if (g_selSet.count(r.node)) g_selSet.erase(r.node);
                    else g_selSet.insert(r.node);
                    g_anchorRow = hoverRow;
                } else {
                    g_selSet.clear();
                    g_selSet.insert(r.node);
                    g_anchorRow = hoverRow;
                }
                g_selNode = r.node;
            }
        }
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            if (r.up || r.node->isDir) Enter(r.node);
            else ShellOpenPath(FullPathOf(r.node));
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            // 右键不改变选择；作用于当前选择集（HTML document.contextmenu）
            std::vector<std::wstring> paths = SelectionPaths();
            if (paths.empty()) paths.push_back(FullPathOf(r.node));
            if (g_st.menu.shellMenu) ShowShellContextMenuAsync(paths);
            else ShowCustomContextMenu(paths);
        }
    }
}

// ------------------------------------------------------------ 键盘 ----
static void HandleKeys() {
    ImGuiIO& io = ImGui::GetIO();

    if (g_showSettings || g_renameOpen || g_newFolderOpen) return;
    if (io.WantTextInput) return;
    int cur = -1;
    for (size_t i = 0; i < g_rows.size(); i++)
        if (g_rows[i].node == g_selNode) { cur = (int)i; break; }

    bool ctrl = io.KeyCtrl, shift = io.KeyShift;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false)) {
        int n = (int)g_rows.size();
        if (n) {
            int i = (cur < 0) ? 0 : std::min(n - 1, cur + 1);
            g_selSet.clear(); g_selSet.insert(g_rows[i].node);
            g_selNode = g_rows[i].node; g_anchorRow = i;
            ScrollRowToView(i);
        }
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false)) {
        int n = (int)g_rows.size();
        if (n) {
            int i = (cur < 0) ? 0 : std::max(0, cur - 1);
            g_selSet.clear(); g_selSet.insert(g_rows[i].node);
            g_selNode = g_rows[i].node; g_anchorRow = i;
            ScrollRowToView(i);
        }
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) {
        if (cur >= 0) {
            const FlatRow& r = g_rows[cur];
            if (r.hasKids && !r.expanded) { g_expanded.insert(r.node); BuildRows(); }
        }
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) {
        if (cur >= 0) {
            const FlatRow& r = g_rows[cur];
            if (r.hasKids && r.expanded) { g_expanded.erase(r.node); BuildRows(); }
            else if (r.depth > 0) {
                int d = r.depth;
                for (int k = cur - 1; k >= 0; k--)
                    if (g_rows[k].depth < d) {
                        g_selSet.clear(); g_selSet.insert(g_rows[k].node);
                        g_selNode = g_rows[k].node; g_anchorRow = k;
                        ScrollRowToView(k);
                        break;
                    }
            }
        }
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
        if (cur >= 0) {
            if (g_rows[cur].node->isDir) Enter(g_rows[cur].node);
            else ShellOpenPath(FullPathOf(g_rows[cur].node));
        }
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) { GoUp(); return; }
    if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) { ShellVerb(L"rename", g_selNode); return; }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
        ShellVerb(shift ? L"delete_perm" : L"delete", g_selNode);
        return;
    }
    if (ctrl && shift && ImGui::IsKeyPressed(ImGuiKey_N, false)) { ShellVerb(L"newfolder", nullptr); return; }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
        g_selSet.clear();
        for (const auto& r : g_rows) g_selSet.insert(r.node);
        g_anchorRow = 0;
        return;
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) { ShellVerb(L"copy", g_selNode); return; }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_X, false)) { ShellVerb(L"cut", g_selNode); return; }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) { ShellVerb(L"paste", nullptr); return; }
}

// ============================================================ 工具栏 =====
static float BtnW(const char* label) {
    return ImGui::CalcTextSize(label).x + 28.0f;
}
static float ComboW(const char* const* items, int n) {
    float w = 0;
    for (int i = 0; i < n; i++) w = std::max(w, ImGui::CalcTextSize(items[i]).x);
    return w + 48.0f;   // 箭头 + 左右内边距
}

static void DrawToolbar(const ImVec2& origin, float winW) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14, 8.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 4));

    const char* lBack = "\xe2\x86\x90 \xe8\xbf\x94\xe5\x9b\x9e";          // ← 返回
    const char* lUp = "\xe2\x86\x91 \xe4\xb8\x8a\xe7\xba\xa7";            // ↑ 上级
    const char* lBrowse = "\xe6\xb5\x8f\xe8\xa7\x88";                      // 浏览
    bool scanning = g_scanning.load();
    const char* lScan = scanning ? "\xe5\x81\x9c\xe6\xad\xa2" : "\xe6\x89\xab\xe6\x8f\x8f";  // 停止/扫描
    static const char* mapItems[5] = {
        "\xe6\x98\xa0\xe5\xb0\x84 g=x",
        "\xe6\x98\xa0\xe5\xb0\x84 g=log\xe2\x82\x82x",
        "\xe6\x98\xa0\xe5\xb0\x84 g=log\xe2\x82\x82\xc2\xb2x",
        "\xe6\x98\xa0\xe5\xb0\x84 g=\xe2\x88\x9ax",
        "\xe6\x98\xa0\xe5\xb0\x84 g=x^\xce\xb1",
    };
    static const char* ordItems[2] = {
        "\xe9\xa1\xba\xe5\xba\x8f \xe5\x85\x88\xe5\x92\x8c\xe2\x86\x92g(\xce\xa3x)",
        "\xe9\xa1\xba\xe5\xba\x8f \xe5\x85\x88 g\xe2\x86\x92\xce\xa3g(x)",
    };
    const char* lTheme = (g_st.theme == 1) ? "\xe6\xb7\xb1\xe8\x89\xb2" : "\xe6\xb5\x85\xe8\x89\xb2";  // 深色/浅色
    bool elevated = IsProcessElevated();
    bool mftOn = elevated && g_wantMftMode;
    const char* lMft = mftOn ? "MFT \xe7\x9b\xb4\xe8\xaf\xbb" : "\xe6\x99\xae\xe9\x80\x9a\xe9\x81\x8d\xe5\x8e\x86";
    const char* lSet = "\xe2\x9a\x99 \xe8\xae\xbe\xe7\xbd\xae";          // ⚙ 设置
    const char* lPath = "\xe8\xb7\xaf\xe5\xbe\x84";                        // 路径

    // ---- 宽度预算（两遍）----
    // 150% DPI 下窗口的逻辑宽度会变窄（1920 物理客户区 → 1265 逻辑），旧写法把路径框
    // 写死 ≥120，导致尾部按钮（主题/MFT/设置）被挤出可视区 —— 用户报「按钮被截断」。
    // 现在的策略：先按完整标签算；放不下就换短标签（← ↑ … MFT/遍历 ⚙）+ 去掉「路径」标题；
    // 尾部组一律右对齐，所以再窄也不会被挤出。
    const float gap = 8.0f;
    float wPow = 0;
    if (g_st.mapKind == 4) wPow = 90.0f + 6.0f + ImGui::CalcTextSize("0.50").x;
    auto widths = [&](const char* lBk, const char* lUpc, const char* lBrs, const char* lMftL,
                      const char* lSetL, bool withCap, float& leftW, float& tailW) {
        int nLeft = 6 + (g_st.mapKind == 4 ? 1 : 0);   // 返回/上级/浏览/扫描/映射/顺序[/α]
        if (withCap) nLeft++;                          // 「路径」标题
        leftW = BtnW(lBk) + BtnW(lUpc) + BtnW(lBrs) + BtnW(lScan) +
                ComboW(mapItems, 5) + ComboW(ordItems, 2) + wPow + gap * (float)(nLeft - 1);
        if (withCap) leftW += ImGui::CalcTextSize(lPath).x + gap;
        tailW = BtnW(lTheme) + BtnW(lMftL) + BtnW(lSetL) + gap * 2.0f;
    };
    float leftW = 0, tailW = 0;
    widths(lBack, lUp, lBrowse, lMft, lSet, true, leftW, tailW);
    bool withCap = true;
    float avail = winW - 24.0f - leftW - tailW - gap;
    if (avail < 90.0f) {                                // 挤不下 → 短标签
        lBack = "\xe2\x86\x90";                         // ←
        lUp = "\xe2\x86\x91";                           // ↑
        lBrowse = "\xe2\x80\xa6";                       // …
        lMft = mftOn ? "MFT" : "\xe9\x81\x8d\xe5\x8e\x86";   // 遍历
        lSet = "\xe2\x9a\x99";                          // ⚙
        withCap = (winW >= 1050.0f);
        widths(lBack, lUp, lBrowse, lMft, lSet, withCap, leftW, tailW);
        avail = winW - 24.0f - leftW - tailW - gap;
    }
    float wBack = BtnW(lBack), wUp = BtnW(lUp), wBrowse = BtnW(lBrowse), wScan = BtnW(lScan);
    float wMap = ComboW(mapItems, 5), wOrd = ComboW(ordItems, 2);
    float wTheme = BtnW(lTheme), wMft = BtnW(lMft), wSet = BtnW(lSet);
    float wCaption = ImGui::CalcTextSize(lPath).x;
    float pathW = std::max(60.0f, avail);

    // 第三条退路：短标签 + 最小路径框仍然放不下（很窄的窗口）→ 尾部控件换到第二行。
    // 宁可工具栏变高，也不让任何按钮被切掉（用户报「按钮被截断」）。
    float row1Need = 12.0f + BtnW(lBack) + gap + BtnW(lUp) + gap +
                     (withCap ? wCaption + gap : 0.0f) + 60.0f + gap +
                     BtnW(lBrowse) + gap + BtnW(lScan) + 12.0f;
    bool wrap = (row1Need > winW);
    // 换行时第二行放：映射 / 顺序 / [α] / 主题 / MFT / 设置
    float tailW2 = ComboW(mapItems, 5) + gap + ComboW(ordItems, 2) + gap + wPow +
                   wTheme + gap + wMft + gap + wSet;
    bool wrap2 = wrap && (12.0f + tailW2 + 12.0f > winW);
    if (wrap2) {   // 第二行也放不下 → 第二行左对齐顺排（让它自然截断，至少第一行完整）
        tailW2 = winW;
    }
    g_toolbarH = wrap ? (TOOLBAR_BG_H + TOOLBAR_ROW_H) : TOOLBAR_BG_H;
    if (wrap) {
        // 换行后第一行只剩「返回/上级/[路径]/输入框/浏览/扫描」→ 输入框能吃满剩余宽度
        float nonInput = row1Need - 60.0f - 24.0f;
        pathW = std::max(60.0f, winW - 24.0f - nonInput);
    }

    float x = origin.x + 12.0f;
    float y = origin.y + 10.0f;
    ImGui::SetCursorScreenPos(ImVec2(x, y));

    ImGui::BeginDisabled(g_navHist.empty());
    if (ImGui::Button(lBack)) GoBack();
    ImGui::EndDisabled();
    x += wBack + gap; ImGui::SameLine(0, gap);

    if (ImGui::Button(lUp)) GoUp();
    x += wUp + gap; ImGui::SameLine(0, gap);

    if (withCap) {
        ImGui::TextUnformatted(lPath);
        x += wCaption + gap;
        ImGui::SameLine(0, gap);
    }

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 8.5f));
    ImGui::SetNextItemWidth(pathW);
    ImGui::InputText("##path", g_pathBuf, sizeof(g_pathBuf));
    ImGui::PopStyleVar();
    x += pathW + gap; ImGui::SameLine(0, gap);

    if (ImGui::Button(lBrowse)) {
        BROWSEINFOW bi{};
        bi.hwndOwner = s_hwnd;
        bi.lpszTitle = L"\xe9\x80\x89\xe6\x8b\xa9\xe8\xa6\x81\xe6\x89\xab\xe6\x8f\x8f\xe7\x9a\x84\xe6\x96\x87\xe4\xbb\xb6\xe5\xa4\xb9";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
        if (pidl) {
            wchar_t buf[MAX_PATH]{};
            if (SHGetPathFromIDListW(pidl, buf)) {
                strcpy_s(g_pathBuf, Utf8(buf).c_str());
                g_st.lastPath = buf;
                SaveSettings(g_st);
                prefs::IniSet(L"lastPath", buf);
            }
            CoTaskMemFree(pidl);
        }
    }
    x += wBrowse + gap; ImGui::SameLine(0, gap);

    {
        ImU32 base = scanning ? ColDanger() : ColAccent();
        ImGui::PushStyleColor(ImGuiCol_Button, V4(base));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, V4(base));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, V4(base));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        if (ImGui::Button(lScan)) {
            if (scanning) {
                g_engine.Cancel();
                g_scanning = false;
                g_progVisible = false;
                g_st0 = L"已停止";
            } else {
                std::wstring p = Wide(std::string(g_pathBuf));
                if (!p.empty()) StartScan(p);
            }
        }
        ImGui::PopStyleColor(4);
    }
    if (wrap) {   // 换行：映射/顺序/[α]/主题/MFT/设置 全部挪到第二行，左对齐顺排
        ImGui::SetCursorScreenPos(ImVec2(origin.x + 12.0f, y + TOOLBAR_ROW_H));
    } else {
        ImGui::SameLine(0, gap);
    }
    ImGui::SetNextItemWidth(wMap);
    {
        int mk = (g_st.mapKind >= 0 && g_st.mapKind <= 4) ? g_st.mapKind : 0;
        if (ImGui::Combo("##map", &mk, mapItems, 5)) {
            g_st.mapKind = mk;
            prefs::SaveUiPrefs();   // 面积算法属于共用界面偏好（Web 版同款键）
            g_layoutStamp++;
        }
    }
    x += wMap + gap; ImGui::SameLine(0, gap);

    ImGui::SetNextItemWidth(wOrd);
    {
        int ok = (g_st.ordKind >= 0 && g_st.ordKind <= 1) ? g_st.ordKind : 0;
        if (ImGui::Combo("##ord", &ok, ordItems, 2)) {
            g_st.ordKind = ok;
            prefs::SaveUiPrefs();
            g_layoutStamp++;
        }
    }
    x += wOrd + gap; ImGui::SameLine(0, gap);

    if (g_st.mapKind == 4) {
        ImGui::SetNextItemWidth(90);
        float a = g_st.powAlpha > 0.01 ? (float)g_st.powAlpha : 0.5f;
        if (ImGui::SliderFloat("##alpha", &a, 0.1f, 1.0f, "")) {
            g_st.powAlpha = a;
            g_layoutStamp++;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) prefs::SaveUiPrefs();
        ImGui::SameLine(0, 6);
        char buf[16];
        snprintf(buf, sizeof(buf), "%.2f", (double)(g_st.powAlpha > 0.01 ? g_st.powAlpha : 0.5));
        ImGui::TextUnformatted(buf);
        x += wPow + gap; ImGui::SameLine(0, gap);
    }

    // 尾部组（主题/MFT/设置）右对齐：窗口再窄也不会被挤出可视区（换行时按顺序排即可）
    if (!wrap) {
        float tailX = origin.x + winW - 12.0f - tailW;
        if (tailX < x + 6.0f) tailX = x + 6.0f;
        ImGui::SetCursorScreenPos(ImVec2(tailX, y));
    }
    if (ImGui::Button(lTheme)) {
        g_st.theme = (g_st.theme == 1) ? 0 : 1;
        prefs::SaveUiPrefs();   // 主题存进共用 ui-prefs.json（Web 版读同一个文件）
        ApplyImGuiTheme();
        g_layoutStamp++;
        TreemapInvalidate();
    }
    x += wTheme + gap; ImGui::SameLine(0, gap);

    {
        ImGui::PushStyleColor(ImGuiCol_Button, V4(mftOn ? ColAccent() : ColBtn()));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, V4(mftOn ? ColAccent() : ColBtnHover()));
        ImGui::PushStyleColor(ImGuiCol_Text, mftOn ? ImVec4(1, 1, 1, 1) : V4(ColDim()));
        if (ImGui::Button(lMft)) {
            bool target = !g_wantMftMode;
            if (target && !elevated) {
                // 切换 MFT 需要管理员：弹 UAC，成功则管理员实例接管（HTML setMft 语义）
                wchar_t exe[MAX_PATH]{};
                GetModuleFileNameW(nullptr, exe, MAX_PATH);
                std::wstring last = g_st.lastPath.empty() ? Wide(std::string(g_pathBuf)) : g_st.lastPath;
                std::wstring params = L"--elevated=1";
                if (!last.empty()) params += L" --autoscan=" + last;
                SHELLEXECUTEINFOW sei{};
                sei.cbSize = sizeof(sei);
                sei.lpVerb = L"runas";
                sei.lpFile = exe;
                sei.lpParameters = params.c_str();
                sei.nShow = SW_SHOWNORMAL;
                LogLine(L"MFT switch: requesting elevation...");
                if (ShellExecuteExW(&sei)) {
                    PostMessageW(s_hwnd, WM_CLOSE, 0, 0);
                } else {
                    g_wantMftMode = false;
                    g_mftMode = 0;
                    prefs::IniSet(L"mft", L"0");
                    LogLine(L"MFT switch: elevation declined, revert to walk");
                }
            } else {
                g_wantMftMode = target;
                g_mftMode = target ? 1 : 0;
                prefs::IniSet(L"mft", std::to_wstring(g_mftMode));
            }
        }
        ImGui::PopStyleColor(3);
    }
    x += wMft + gap; ImGui::SameLine(0, gap);

    if (ImGui::Button(lSet)) {
        g_stEdit = g_st;
        g_setHint.clear();
        g_settingsDirty = false;
        g_showSettings = true;
    }
    (void)x;
    ImGui::PopStyleVar(2);
}

// ============================================================ 面包屑 =====
static void DrawCrumbs(const ImVec2& origin, float winW, float y) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(origin.x, y), ImVec2(origin.x + winW, y + CRUMB_H), ColBg());
    dl->AddLine(ImVec2(origin.x, y + CRUMB_H), ImVec2(origin.x + winW, y + CRUMB_H), ColLine());
    if (!g_current) return;
    std::vector<ScanNode*> chain;
    for (ScanNode* n = g_current; n; n = n->parent) chain.push_back(n);
    std::reverse(chain.begin(), chain.end());

    ImFont* f = g_fontUi ? g_fontUi : ImGui::GetFont();
    float x = origin.x + 12.0f;
    float ty = y + (CRUMB_H - 12.0f) / 2.0f - 1.0f;
    dl->PushClipRect(ImVec2(origin.x, y), ImVec2(origin.x + winW, y + CRUMB_H), true);
    for (size_t i = 0; i < chain.size(); i++) {
        if (i) {
            const char* sep = "\xe2\x80\xba";   // ›
            dl->AddText(f, 12.0f, ImVec2(x + 4.0f, ty), AlphaOf(ColDim(), 153), sep);
            x += 4.0f + f->CalcTextSizeA(12.0f, FLT_MAX, 0.0f, sep).x + 4.0f;
        }
        std::string nm = Ellipsize(Utf8(chain[i]->name).c_str(), winW - (x - origin.x) - 40.0f, f, 12.0f);
        ImVec2 sz = f->CalcTextSizeA(12.0f, FLT_MAX, 0.0f, nm.c_str());
        ImVec2 a(x, ty), b(x + sz.x, ty + 12.0f);
        bool hov = ImGui::IsMouseHoveringRect(a, b, false) && ImGui::IsWindowHovered();
        if (hov) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            dl->AddLine(ImVec2(a.x, b.y + 1), ImVec2(b.x, b.y + 1), ColText());
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) Enter(chain[i]);
        }
        dl->AddText(f, 12.0f, ImVec2(x, ty), ColText(), nm.c_str());
        x += sz.x + 8.0f;
    }
    dl->PopClipRect();
}

// ============================================================ 状态栏 =====
static void DrawStatusBar(const ImVec2& origin, float winW, float winH) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float y = origin.y + winH - STATUS_H;
    dl->AddRectFilled(ImVec2(origin.x, y), ImVec2(origin.x + winW, origin.y + winH), ColPanel());
    dl->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + winW, y), ColLine());

    ImFont* fs = g_fontSm ? g_fontSm : ImGui::GetFont();
    float ty = y + (STATUS_H - 12.0f) / 2.0f - 1.0f;
    float x = origin.x + 12.0f;
    auto span = [&](const std::wstring& s) {
        if (s.empty()) return;
        std::string u = Utf8(s);
        dl->AddText(fs, 12.0f, ImVec2(x, ty), ColDim(), u.c_str());
        x += fs->CalcTextSizeA(12.0f, FLT_MAX, 0.0f, u.c_str()).x + 24.0f;
    };
    span(g_st0);
    span(g_st1);
    span(g_st2);

    // 进度胶囊：110×5 圆角条 + 11px 文字（仅扫描中显示）
    if (g_progVisible) {
        const float barW = 110.0f, barH = 5.0f;
        std::string txt = Utf8(g_progText);
        float txtW = fs->CalcTextSizeA(11.0f, FLT_MAX, 0.0f, txt.c_str()).x;
        float totalW = barW + 8.0f + txtW;
        float bx = origin.x + winW - 12.0f - totalW;
        float by = y + (STATUS_H - barH) / 2.0f;
        dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + barW, by + barH),
                          AlphaOf(ColText(), 41), 3.0f);
        // 40% 宽度循环滑动（@keyframes prog-slide 1.1s ease-in-out infinite）
        double t = fmod(ImGui::GetTime() / 1.1, 1.0);
        double e = (t < 0.5) ? (2 * t * t) : (1.0 - pow(-2 * t + 2, 2) / 2.0);
        float fx = bx + (float)((-0.4 + 1.4 * e) * barW);
        dl->PushClipRect(ImVec2(bx, by), ImVec2(bx + barW, by + barH), true);
        dl->AddRectFilled(ImVec2(fx, by), ImVec2(fx + barW * 0.4f, by + barH), ColAccent(), 3.0f);
        dl->PopClipRect();
        dl->AddText(fs, 11.0f, ImVec2(bx + barW + 8.0f, y + (STATUS_H - 11.0f) / 2.0f - 1.0f),
                    ColDim(), txt.c_str());
    }
}

// ====================================================== D3D11 / 窗口 =====
static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static IDXGISwapChain* g_swap = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;

static bool CreateD3D11(HWND hwnd, int w, int h) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = (UINT)w;
    sd.BufferDesc.Height = (UINT)h;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                               levels, 2, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev,
                                               nullptr, &g_ctx);
    return SUCCEEDED(hr);
}
static void CreateRTV() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) && back) {
        g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

// 后缓冲必须与客户区**完全一致**：否则 DXGI 会把 1920×1200 的缓冲拉伸到
// 1897×1143 的客户区（横向 ×0.988、纵向 ×0.953 的非等比缩放）→ 画面被重采样发糊，
// 而且鼠标坐标与画面按不同比例对应 → 点在哪儿和看到的不一致（越靠边越明显）。
// 首次 WM_SIZE 发生在 CreateWindowEx 里（那时 D3D 还没建），会被漏掉，所以这里兜底。
static void EnsureBackBufferSize() {
    if (!g_swap || !g_ctx) return;
    RECT rc{};
    GetClientRect(s_hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    static int lastW = 0, lastH = 0;
    if (w == lastW && h == lastH) return;
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) && back) {
        D3D11_TEXTURE2D_DESC d{};
        back->GetDesc(&d);
        back->Release();
        if ((int)d.Width != w || (int)d.Height != h) {
            g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
            if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
            if (SUCCEEDED(g_swap->ResizeBuffers(0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, 0)))
                CreateRTV();
            LogLine(L"backbuffer resized to " + std::to_wstring(w) + L"x" + std::to_wstring(h));
        }
    }
    lastW = w;
    lastH = h;
}
static void CleanupD3D11() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_swap) { g_swap->Release(); g_swap = nullptr; }
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
}

// 后缓冲 → 24bit BMP（自动化截图自检用）
static bool SaveBackBufferBMP(const wchar_t* path) {
    if (!g_swap) return false;
    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) || !back) return false;
    D3D11_TEXTURE2D_DESC d{};
    back->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ID3D11Texture2D* stg = nullptr;
    bool ok = false;
    if (SUCCEEDED(g_dev->CreateTexture2D(&sd, nullptr, &stg)) && stg) {
        g_ctx->CopyResource(stg, back);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(g_ctx->Map(stg, 0, D3D11_MAP_READ, 0, &m))) {
            int W = (int)d.Width, H = (int)d.Height;
            int rowBytes = ((W * 3 + 3) / 4) * 4;
            DWORD dataSize = (DWORD)(rowBytes * H);
            BITMAPFILEHEADER fh{};
            BITMAPINFOHEADER ih{};
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof(fh) + sizeof(ih);
            fh.bfSize = fh.bfOffBits + dataSize;
            ih.biSize = sizeof(ih);
            ih.biWidth = W;
            ih.biHeight = H;
            ih.biPlanes = 1;
            ih.biBitCount = 24;
            ih.biCompression = BI_RGB;
            ih.biSizeImage = dataSize;
            FILE* fp = nullptr;
            if (_wfopen_s(&fp, path, L"wb") == 0 && fp) {
                fwrite(&fh, sizeof(fh), 1, fp);
                fwrite(&ih, sizeof(ih), 1, fp);
                std::vector<BYTE> row((size_t)rowBytes, 0);
                for (int y = H - 1; y >= 0; y--) {
                    const BYTE* src = (const BYTE*)m.pData + (size_t)y * m.RowPitch;
                    for (int x = 0; x < W; x++) {
                        row[x * 3 + 0] = src[x * 4 + 2];
                        row[x * 3 + 1] = src[x * 4 + 1];
                        row[x * 3 + 2] = src[x * 4 + 0];
                    }
                    fwrite(row.data(), 1, (size_t)rowBytes, fp);
                }
                fclose(fp);
                ok = true;
            }
            g_ctx->Unmap(stg, 0);
        }
        stg->Release();
    }
    back->Release();
    return ok;
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 0;
    switch (msg) {
        case WM_SIZE:
            if (g_swap) {
                int w = LOWORD(lp), h = HIWORD(lp);
                if (w > 0 && h > 0) {
                    g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
                    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
                    g_swap->ResizeBuffers(0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, 0);
                    CreateRTV();
                }
            }
            return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        case WM_DPICHANGED: {
            // 跨显示器（不同缩放）时重算 UI 缩放 + 按建议矩形调整窗口
            UINT dpi = HIWORD(wp);
            g_uiScale = (dpi >= 96) ? (float)dpi / 96.0f : 1.0f;
            RECT* sug = (RECT*)lp;
            if (sug)
                SetWindowPos(hwnd, nullptr, sug->left, sug->top, sug->right - sug->left,
                             sug->bottom - sug->top, SWP_NOZORDER | SWP_NOACTIVATE);
            LoadFonts();
            ApplyImGuiTheme();
            g_layoutStamp++;
            LogLine(L"DPI changed -> uiScale=" + std::to_wstring((int)(g_uiScale * 100)) + L"%");
            return 0;
        }
        default: return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

// ============================================================ 主渲染 =====
// DPI 适配：把 ImGui 的坐标空间统一成【逻辑像素】，并让渲染后端按 DPI 放大到物理像素。
//   · io.DisplaySize            = 物理客户区 / uiScale  → 布局/字号都是逻辑像素（= CSS px）
//   · io.DisplayFramebufferScale = uiScale              → 后端 viewport/scissor 放大，
//                                                        且 ImGui 用它当字体 RasterizerDensity
//   · 鼠标：后端给的是物理客户区坐标 → 换算成逻辑坐标（delta 因此也自动落在逻辑空间）
static void ApplyDpiToImGui() {
    ImGuiIO& io = ImGui::GetIO();
    float s = (g_uiScale > 0.0f) ? g_uiScale : 1.0f;
    io.DisplaySize = ImVec2(io.DisplaySize.x / s, io.DisplaySize.y / s);
    io.DisplayFramebufferScale = ImVec2(s, s);
    if (s == 1.0f) return;
    POINT pt{};
    GetCursorPos(&pt);
    ScreenToClient(s_hwnd, &pt);
    RECT rc{};
    GetClientRect(s_hwnd, &rc);
    POINT cl = pt;
    ImVec2 mp((float)cl.x, (float)cl.y);
    if (!PtInRect(&rc, cl)) mp = ImVec2(-FLT_MAX, -FLT_MAX);
    if (g_dbgClick.x > -1e8f) mp = g_dbgClick;        // 自检：模拟点击位置（物理坐标）
    else if (g_dbgMouse.x > -1e8f) mp = g_dbgMouse;   // 自检：模拟悬停位置（物理坐标）
    if (g_frameNo >= 55 && g_dbgClick2.x > -1e8f) mp = g_dbgClick2;   // 第二次点击的位置
    if (g_frameNo >= 25 && g_dbgDblClick.x > -1e8f) mp = g_dbgDblClick;   // 双击的位置
    io.AddMousePosEvent(mp.x / s, mp.y / s);
}

static void RenderFrame() {
    ImGuiIO& io = ImGui::GetIO();
    g_frameNo++;
    // 自检用的鼠标事件注入（各自独立判断，别互相套住）
    if (g_dbgClick.x > -1e8f) {                      // --click=x,y：第 30 帧点一下
        if (g_frameNo == 30) io.AddMouseButtonEvent(0, true);
        if (g_frameNo == 31) io.AddMouseButtonEvent(0, false);
    }
    if (g_dbgClick2.x > -1e8f) {                     // --click2=x,y：第 60 帧再点一下
        if (g_frameNo == 60) io.AddMouseButtonEvent(0, true);
        if (g_frameNo == 61) io.AddMouseButtonEvent(0, false);
    }
    if (g_dbgDblClick.x > -1e8f) {                   // --dblclick=x,y：真双击（间隔 4 帧）
        if (g_frameNo == 30 || g_frameNo == 34) io.AddMouseButtonEvent(0, true);
        if (g_frameNo == 31 || g_frameNo == 35) io.AddMouseButtonEvent(0, false);
    }
    if (g_dbgClick.x > -1e8f && g_dbgWheelTicks > 0 && g_frameNo >= 40 && (g_frameNo % 3) == 0) {
        io.AddMouseWheelEvent(0.0f, 1.0f);
        g_dbgWheelTicks--;
    }
    if (!g_dbgKey.empty() && g_frameNo == 35) {
        ImGuiKey k = ImGuiKey_None;
        if (g_dbgKey == L"F2") k = ImGuiKey_F2;
        else if (g_dbgKey == L"Delete") k = ImGuiKey_Delete;
        else if (g_dbgKey == L"Enter") k = ImGuiKey_Enter;
        else if (g_dbgKey == L"Backspace") k = ImGuiKey_Backspace;
        else if (g_dbgKey == L"CtrlA") { io.AddKeyEvent((ImGuiKey)ImGuiMod_Ctrl, true); k = ImGuiKey_A; }
        else if (g_dbgKey == L"CtrlN") { io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiMod_Shift, true); k = ImGuiKey_N; }
        if (k != ImGuiKey_None) io.AddKeyEvent(k, true);
    }
    if (!g_dbgKey.empty() && g_frameNo == 36) {
        if (g_dbgKey == L"F2") io.AddKeyEvent(ImGuiKey_F2, false);
        else if (g_dbgKey == L"Delete") io.AddKeyEvent(ImGuiKey_Delete, false);
        else if (g_dbgKey == L"Enter") io.AddKeyEvent(ImGuiKey_Enter, false);
        else if (g_dbgKey == L"Backspace") io.AddKeyEvent(ImGuiKey_Backspace, false);
        else if (g_dbgKey == L"CtrlA") { io.AddKeyEvent(ImGuiKey_A, false); io.AddKeyEvent((ImGuiKey)ImGuiMod_Ctrl, false); }
        else if (g_dbgKey == L"CtrlN") { io.AddKeyEvent(ImGuiKey_N, false); io.AddKeyEvent(ImGuiMod_Ctrl, false); io.AddKeyEvent(ImGuiMod_Shift, false); }
    }
    if (g_dbgScroll >= 0.0f && !g_rows.empty()) {
        s_pendingScrollY = g_dbgScroll;
        g_dbgScroll = -1.0f;
    }
    ApplyDpiToImGui();
    ImGui::NewFrame();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 origin = vp->Pos;
    float W = vp->Size.x, H = vp->Size.y;

    ImGui::SetNextWindowPos(origin);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoSavedSettings);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    // ---- 工具栏 ----
    dl->AddRectFilled(origin, ImVec2(origin.x + W, origin.y + g_toolbarH), ColPanel());
    dl->AddLine(ImVec2(origin.x, origin.y + g_toolbarH),
                ImVec2(origin.x + W, origin.y + g_toolbarH), ColLine());
    DrawToolbar(origin, W);

    // ---- 面包屑 ----
    float crumbY = origin.y + g_toolbarH + 1.0f;
    DrawCrumbs(origin, W, crumbY);

    // ---- 主体 ----
    float mainY = crumbY + CRUMB_H + 1.0f;
    float mainH = (origin.y + H - STATUS_H - 1.0f) - mainY;
    if (mainH < 60.0f) mainH = 60.0f;
    float mainW = W;
    float listW = floorf(mainW * std::min(SPLIT_MAX, std::max(SPLIT_MIN, g_splitFrac)));
    float rightX = origin.x + listW + SPLIT_W;
    float rightW = mainW - listW - SPLIT_W;
    if (rightW < 80.0f) rightW = 80.0f;

    // 左栏底
    dl->AddRectFilled(ImVec2(origin.x, mainY), ImVec2(origin.x + listW, mainY + mainH), ColBg());

    // 表头（非滚动区）
    ImGui::SetCursorScreenPos(ImVec2(origin.x, mainY));
    DrawListHeader(dl, ImVec2(origin.x, mainY), listW, ColsLayout());

    // 列表滚动区
    ImGui::SetCursorScreenPos(ImVec2(origin.x, mainY + HEAD_H));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("##list", ImVec2(listW, mainH - HEAD_H), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoMove);
    DrawListPanel(ImVec2(listW, mainH - HEAD_H));
    ImGui::EndChild();
    ImGui::PopStyleVar();

    // ---- 分栏拖拽条 ----
    {
        ImVec2 a(origin.x + listW, mainY);
        ImVec2 b(a.x + SPLIT_W, mainY + mainH);
        dl->AddRectFilled(a, b, ColBg());
        bool hov = ImGui::IsMouseHoveringRect(a, b, false);
        static bool dragging = false;
        ImU32 lc = ColLine();
        if (hov || dragging) { lc = ColAccent(); ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW); }
        dl->AddRectFilled(ImVec2(a.x + 2, a.y), ImVec2(a.x + 3, b.y), lc);
        if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) dragging = true;
        if (dragging) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                float frac = (ImGui::GetIO().MousePos.x - origin.x) / mainW;
                g_splitFrac = std::min(SPLIT_MAX, std::max(SPLIT_MIN, frac));
            } else {
                dragging = false;
                prefs::SaveUiPrefs();
            }
        }
    }

    // ---- 右侧树图 ----
    ImGui::SetCursorScreenPos(ImVec2(rightX, mainY));
    ImGui::BeginChild("##tm", ImVec2(rightW, mainH), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    DrawTreemapPanel(ImVec2(rightW, mainH));
    ImGui::EndChild();

    // ---- 表头列宽拖拽（命中在手柄 ±3px）----
    {
        ColGeom cg = ColsLayout();
        ImVec2 mouse = ImGui::GetIO().MousePos;
        static int dragCol = -1;
        static float startX = 0, startVal = 0;
        auto gripRange = [&](float edge, bool* hit) {
            float gx = origin.x + edge;
            *hit = (mouse.y >= mainY && mouse.y < mainY + HEAD_H && mouse.x >= gx - 3 &&
                    mouse.x <= gx + 3);
        };
        if (dragCol < 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            bool hit = false;
            gripRange(ARROW + cg.nameW, &hit);
            if (hit) { dragCol = 0; startX = mouse.x; startVal = cg.nameW; }
            else {
                for (int i = 0; i < 4 && dragCol < 0; i++) {
                    gripRange(cg.x[i] + cg.w[i], &hit);
                    if (hit) { dragCol = i + 1; startX = mouse.x; startVal = cg.w[i]; }
                }
            }
            // 表头点击排序（未拖拽手柄时）
            if (dragCol < 0 && mouse.y >= mainY && mouse.y < mainY + HEAD_H && mouse.x < origin.x + listW) {
                float mx = mouse.x - origin.x;
                auto pick = [&](float x, float w, int key) {
                    if (mx >= x && mx < x + w) {
                        if (g_sortKey == key) g_sortAsc = !g_sortAsc;
                        else { g_sortKey = key; g_sortAsc = (key == 0); }
                        BuildRows();
                        return true;
                    }
                    return false;
                };
                if (!pick(cg.nameX, cg.nameW, 0))
                    for (int i = 0; i < 4; i++)
                        if (pick(cg.x[i], cg.w[i], i + 1)) break;
            }
        }
        if (dragCol == 0) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                g_nameW = std::max(NAME_MIN, startVal + (mouse.x - startX));
            } else { dragCol = -1; prefs::SaveUiPrefs(); }
        } else if (dragCol > 0) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                const float mins[4] = { 56, 48, 56, 56 };
                g_colW[dragCol - 1] = std::max(mins[dragCol - 1], startVal + (mouse.x - startX));
            } else { dragCol = -1; prefs::SaveUiPrefs(); }
        } else {
            // 手柄 hover 高亮
            bool hit = false;
            gripRange(ARROW + cg.nameW, &hit);
            for (int i = 0; i < 4 && !hit; i++) gripRange(cg.x[i] + cg.w[i], &hit);
            if (hit) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
    }

    DrawStatusBar(origin, W, H);
    HandleKeys();
    // 重命名 / 新建文件夹对话框：OpenPopup 与 BeginPopupModal 必须在同一窗口作用域内
    DrawInputDialogs();
    ImGui::End();

    DrawSettingsModal();
}

// ============================================================ 入口 =====
static int Run(const wchar_t* cmdLine) {
    std::wstring args = cmdLine ? cmdLine : L"";
    auto argVal = [&](const wchar_t* key) -> std::wstring {
        std::wstring pat = std::wstring(key) + L"=";
        size_t p = args.find(pat);
        if (p == std::wstring::npos) return L"";
        std::wstring v = args.substr(p + pat.size());
        size_t sp = v.find(L" --");
        if (sp != std::wstring::npos) v = v.substr(0, sp);
        while (!v.empty() && (v.front() == L' ' || v.front() == L'"')) v.erase(v.begin());
        while (!v.empty() && (v.back() == L' ' || v.back() == L'"')) v.pop_back();
        return v;
    };
    bool elevatedArg = args.find(L"--elevated") != std::wstring::npos;
    std::wstring autoscan = argVal(L"--autoscan");
    std::wstring shot = argVal(L"--shot");
    int shotFrames = 40;
    {
        std::wstring f = argVal(L"--frames");
        if (!f.empty()) shotFrames = _wtoi(f.c_str());
        if (shotFrames < 2) shotFrames = 2;
    }
    {
        std::wstring hp = argVal(L"--hoverpos");
        size_t comma = hp.find(L',');
        if (comma != std::wstring::npos) {
            g_dbgMouse.x = (float)_wtof(hp.substr(0, comma).c_str());
            g_dbgMouse.y = (float)_wtof(hp.substr(comma + 1).c_str());
        }
        std::wstring sc = argVal(L"--scroll");
        if (!sc.empty()) g_dbgScroll = (float)_wtof(sc.c_str());
    }
    // 自检：到第 25 帧弹一次右键菜单（验证壳菜单 COM 通路）
    std::wstring ctxmenu = argVal(L"--ctxmenu");
    bool ctxmenuDone = false;
    bool savePrefsOnce = args.find(L"--saveprefs") != std::wstring::npos;
    {
        std::wstring cl = argVal(L"--click");
        size_t comma = cl.find(L',');
        if (comma != std::wstring::npos) {
            g_dbgClick.x = (float)_wtof(cl.substr(0, comma).c_str());
            g_dbgClick.y = (float)_wtof(cl.substr(comma + 1).c_str());
        }
        std::wstring c2 = argVal(L"--click2");
        size_t comma2 = c2.find(L',');
        if (comma2 != std::wstring::npos) {
            g_dbgClick2.x = (float)_wtof(c2.substr(0, comma2).c_str());
            g_dbgClick2.y = (float)_wtof(c2.substr(comma2 + 1).c_str());
        }
        std::wstring dc = argVal(L"--dblclick");
        size_t commaD = dc.find(L',');
        if (commaD != std::wstring::npos) {
            g_dbgDblClick.x = (float)_wtof(dc.substr(0, commaD).c_str());
            g_dbgDblClick.y = (float)_wtof(dc.substr(commaD + 1).c_str());
        }
        std::wstring wh = argVal(L"--wheel");
        if (!wh.empty()) g_dbgWheelTicks = _wtoi(wh.c_str());
        std::wstring kb = argVal(L"--key");
        if (!kb.empty()) g_dbgKey = kb;
    }
    LogLine(L"imgui run cmd=[" + args + L"]");

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    // 初始窗口尺寸按 DPI 放大（Web 版同款做法：否则高 DPI 下窗口显得很小）
    {
        typedef UINT(WINAPI * GetDpiForSystemFn)();
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        auto gs = u32 ? (GetDpiForSystemFn)GetProcAddress(u32, "GetDpiForSystem") : nullptr;
        UINT dpi = gs ? gs() : 96;
        if (dpi < 96) dpi = 96;
        g_uiScale = (float)dpi / 96.0f;
    }
    int winW = (int)(1280 * g_uiScale), winH = (int)(800 * g_uiScale);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCE(101));
    wc.hIconSm = LoadIconW(hInst, MAKEINTRESOURCE(101));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(30, 34, 40));
    wc.lpszClassName = L"DiskMateImguiMain";
    RegisterClassExW(&wc);
    s_hwnd = CreateWindowExW(0, wc.lpszClassName, L"DiskMate — 磁盘空间分析",
                             WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             CW_USEDEFAULT, CW_USEDEFAULT, winW, winH, nullptr, nullptr, hInst,
                             nullptr);
    if (!s_hwnd) { LogLine(L"CreateWindow failed"); return 1; }
    if (!CreateD3D11(s_hwnd, winW, winH)) { LogLine(L"D3D11 create failed"); return 1; }
    CreateRTV();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    ImGui_ImplWin32_Init(s_hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
    LoadFonts();

    LoadSettings(g_st);
    InitDefaultExtMap();
    prefs::LoadUiPrefs();          // 共用 ui-prefs.json（Web 版同款 schema）
    g_mftMode = _wtoi(prefs::IniGet(L"mft", L"2").c_str());   // 共用 diskmate.ini
    g_wantMftMode = (g_mftMode != 0);
    if (g_mftMode < 0 || g_mftMode > 2) g_mftMode = 0;
    // 管理员启动且持久化模式为 0 → 置为 2（自动，按钮显示 MFT 直读），与 HTML 宿主一致
    if (IsProcessElevated() && g_mftMode == 0) {
        g_mftMode = 2;
        g_wantMftMode = true;
        prefs::IniSet(L"mft", L"2");
        LogLine(L"admin startup: mft auto-enabled (mode 2)");
    }
    ApplyImGuiTheme();
    {
        std::wstring last = prefs::IniGet(L"lastPath", g_st.lastPath);
        if (!last.empty() && autoscan.empty()) strcpy_s(g_pathBuf, Utf8(last).c_str());
    }
    if (!autoscan.empty()) {
        strcpy_s(g_pathBuf, Utf8(autoscan).c_str());
        StartScan(autoscan);
    }
    if (args.find(L"--opensettings") != std::wstring::npos) {
        g_stEdit = g_st;
        g_showSettings = true;
    }
    LogLine(L"imgui front start elevated=" + std::to_wstring(IsProcessElevated() ? 1 : 0));
    // DPI 诊断：确认进程是 PerMonitorV2 且拿到真实缩放（字体按 DPI 放大才不糊）
    {
        UINT sdpi = 0, wdpi = 0;
        typedef UINT(WINAPI * GetDpiForSystemFn)();
        typedef UINT(WINAPI * GetDpiForWindowFn)(HWND);
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        auto gs = u32 ? (GetDpiForSystemFn)GetProcAddress(u32, "GetDpiForSystem") : nullptr;
        auto gw = u32 ? (GetDpiForWindowFn)GetProcAddress(u32, "GetDpiForWindow") : nullptr;
        if (gs) sdpi = gs();
        if (gw) wdpi = gw(s_hwnd);
        if (wdpi >= 96) g_uiScale = (float)wdpi / 96.0f;
        LogLine(L"DPI system=" + std::to_wstring(sdpi) + L" window=" + std::to_wstring(wdpi) +
                L" uiScale=" + std::to_wstring((int)(g_uiScale * 100.0f)) + L"%");
    }
    ShowWindow(s_hwnd, SW_SHOWNORMAL);
    UpdateWindow(s_hwnd);

    bool done = false;
    int frame = 0;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        // 工作线程结果 → UI 线程接管
        if (s_pendReady.exchange(false)) ApplyScanResult();
        if (s_pendFail.exchange(false)) {
            std::wstring reason;
            { std::lock_guard<std::mutex> lk(s_pendMx); reason = s_pendFailReason; }
            g_scanning = false;
            g_progVisible = false;
            g_st0 = L"扫描失败：" + reason;
            LogLine(L"SCAN FAIL " + reason);
        }
        // 进度文字（速度 = 差分 / 耗时，HTML setProgress）
        if (g_progDirty.exchange(false)) {
            ULONGLONG items = g_progItems.load(), bytes = g_progBytes.load();
            if (s_progT0 == 0.0) { s_progT0 = ImGui::GetTime(); s_progI0 = items; s_progB0 = bytes; }
            double dt = std::max(0.2, ImGui::GetTime() - s_progT0);
            ULONGLONG ir = (ULONGLONG)((double)(items - s_progI0) / dt);
            ULONGLONG br = (ULONGLONG)((double)(bytes - s_progB0) / dt);
            g_progText = Wide(FmtCount(items)) + L" 项 · " + FmtSizeW(bytes) + L" · " +
                         Wide(FmtCount(ir)) + L" 项/s · " + FmtSizeW(br) + L"/s";
        }

        EnsureBackBufferSize();   // 后缓冲跟客户区对齐（否则 DXGI 拉伸 → 糊 + 鼠标错位）
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        RenderFrame();
        if (savePrefsOnce && frame >= 20) {
            savePrefsOnce = false;
            prefs::SaveUiPrefs();
            LogLine(L"saveprefs requested");
        }
        if (!ctxmenu.empty() && !ctxmenuDone && frame >= 25) {
            ctxmenuDone = true;
            std::vector<std::wstring> v{ ctxmenu };
            if (g_st.menu.shellMenu) ShowShellContextMenuAsync(v);
            else ShowCustomContextMenu(v);
            LogLine(L"ctxmenu requested for " + ctxmenu);
        }
        ImGui::Render();
        float clear[4] = { 0.10f, 0.11f, 0.13f, 1.0f };
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        frame++;
        if (!shot.empty() && frame >= shotFrames) {
            // 诊断：把当前帧的绘制命令列出（定位「画不全」是几何问题还是裁剪问题）
            if (ImDrawData* dd = ImGui::GetDrawData()) {
                LogLine(L"DRAWDATA lists=" + std::to_wstring(dd->CmdListsCount) + L" vtx=" +
                        std::to_wstring(dd->TotalVtxCount) + L" idx=" +
                        std::to_wstring(dd->TotalIdxCount) + L" display=" +
                        std::to_wstring((int)dd->DisplaySize.x) + L"x" +
                        std::to_wstring((int)dd->DisplaySize.y));
                for (int li = 0; li < dd->CmdListsCount; li++) {
                    const ImDrawList* cl = dd->CmdLists[li];
                    LogLine(L"  list" + std::to_wstring(li) + L" vtx=" +
                            std::to_wstring(cl->VtxBuffer.Size) + L" idx=" +
                            std::to_wstring(cl->IdxBuffer.Size) + L" cmds=" +
                            std::to_wstring(cl->CmdBuffer.Size));
                    int shown = 0;
                    for (const ImDrawCmd& c : cl->CmdBuffer) {
                        if (c.ElemCount == 0) continue;
                        if (shown++ > 8) break;
                        LogLine(L"    cmd elem=" + std::to_wstring(c.ElemCount) + L" clip=" +
                                std::to_wstring((int)c.ClipRect.x) + L"," +
                                std::to_wstring((int)c.ClipRect.y) + L"-" +
                                std::to_wstring((int)c.ClipRect.z) + L"," +
                                std::to_wstring((int)c.ClipRect.w) + L" vtxOfs=" +
                                std::to_wstring(c.VtxOffset));
                    }
                }
            }
            bool ok = SaveBackBufferBMP(shot.c_str());
            LogLine(std::wstring(L"SHOT ") + (ok ? L"ok " : L"FAIL ") + shot);
            done = true;
            break;
        }
        g_swap->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupD3D11();
    LogLine(L"imgui front exit");
    return 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR cmdLine, int) {
    // 启动提权：默认要求管理员（与 HTML 宿主一致）；用户拒绝则普通模式继续
    {
        LPWSTR cl = GetCommandLineW();
        bool already = wcsstr(cl, L"--elevated=1") != nullptr;
        if (!already && !IsProcessElevated()) {
            wchar_t exe[MAX_PATH]{};
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            LPWSTR a = cl;
            if (*a == L'"') { a++; while (*a && *a != L'"') a++; if (*a == L'"') a++; }
            else while (*a && *a != L' ') a++;
            while (*a == L' ') a++;
            std::wstring params = a;
            if (!params.empty()) params += L" ";
            params += L"--elevated=1";
            SHELLEXECUTEINFOW sei{};
            sei.cbSize = sizeof(sei);
            sei.lpVerb = L"runas";
            sei.lpFile = exe;
            sei.lpParameters = params.c_str();
            sei.nShow = SW_SHOWNORMAL;
            if (ShellExecuteExW(&sei)) return 0;
        }
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    OleInitialize(nullptr);   // 壳菜单（IContextMenu）需要 OLE
    int rc = Run(cmdLine);
    OleUninitialize();
    CoUninitialize();
    return rc;
}
