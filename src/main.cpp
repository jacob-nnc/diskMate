// DiskMate — 类 WizTree 的磁盘空间分析工具（原生 Win32）
// 核心设计：UI 线程永不阻塞。
//   - 扫描、IO、Shell 扩展加载全部在后台线程完成，通过 PostMessage 回传。
//   - 右键菜单先瞬间弹出基础项，耗时选项（含资源管理器 Shell 扩展）由
//     后台线程构建后异步插入/接管，从根本上修复 WizTree 右键卡死的问题。
//   - 设置与右键菜单项全部由 config.json 双向绑定（数据驱动解耦）。

#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef _WIN32_IE
#define _WIN32_IE 0x0600
#endif
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlguid.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwchar>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "scan_engine.h"
#include "filelist.h"

// ---------- 日志（恒写，UTF-8；用户明确日志不做环境变量门控） ----------
void LogLine(const std::wstring& s) {
    static FILE* fp = nullptr;
    if (!fp) {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir = exe;
        size_t sl = dir.find_last_of(L"\\/");
        dir = (sl == std::wstring::npos) ? L"" : dir.substr(0, sl + 1);
        std::wstring p = dir + L"diskmate.log";
        _wfopen_s(&fp, p.c_str(), L"a, ccs=UTF-8");
    }
    if (!fp) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fwprintf(fp, L"[%02d:%02d:%02d.%03d] %ls\n", st.wHour, st.wMinute, st.wSecond,
             st.wMilliseconds, s.c_str());
    fflush(fp);
}

#include <commctrl.h>
#include <windowsx.h>

#include "scanner.h"
#include "settings.h"
#include "theme.h"
#include "treemap.h"
#include "utils.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

#ifndef SB_SETTEXTCOLOR
#define SB_SETTEXTCOLOR (WM_USER + 26)  // MinGW 未定义，手动补
#endif

// ---------- 控件 ID ----------
#define IDC_PATH_EDIT 101
#define IDC_BROWSE 102
#define IDC_SCAN 103
#define IDC_STOP 104
#define IDC_UP 105
#define IDC_REFRESH 106
#define IDC_PATH_LABEL 107
#define IDC_LIST 108
#define IDC_STATUS 109
#define IDC_SETTINGS 110
#define IDC_THEME 112   // 工具栏主题切换（浅色/深色，与 Web 版一致）
#define IDC_MAP 117     // 面积映射 combo（0=x 1=log₂ 2=log₂² 3=√x 4=x^α）
#define IDC_ORD 118     // 顺序 combo（0=先和后 g 1=先 g 后和）
#define IDC_POWA 119    // 幂指数 α 滑杆（mapKind==4 时显示）
#define IDC_BACK 116    // 工具栏后退
#define IDC_UPROW 111   // 固定「..」行自绘窗口

// ---------- 本项目右键菜单命令（200~218，数据驱动，config.json 可关） ----------
#define IDM_OPEN 200
#define IDM_EXPLORE 201
#define IDM_COPYPATH 202
#define IDM_COPYSIZE 203
#define IDM_LOADING 204
#define IDM_DELETE 205
#define IDM_RESCAN 206
#define IDM_PROPS 207
#define IDM_ENTER 208
#define IDM_TOTREE 209
#define IDM_SELECT_PARENT 210
#define IDM_TERMINAL 211
#define IDM_COPYNAME 212
#define IDM_RENAME 213
#define IDM_GOUP 214
#define IDM_BACK 215
#define IDM_FORWARD 216
#define IDM_DELETE_FOREVER 217
#define IDM_LOCATE_TREEMAP 218

// 资源管理器 Shell 扩展菜单 ID 区间（由 QueryContextMenu 分配）
#define IDM_SHELL_FIRST 3000
#define IDM_SHELL_LAST 4000

// 自定义线程消息（PostMessage，非阻塞）
#define WM_APP_SCAN_PROGRESS (WM_APP + 1)
#define WM_APP_SCAN_DONE (WM_APP + 2)
#define WM_APP_MENU_READY (WM_APP + 3)
#define WM_APP_SHELL_MENU_READY (WM_APP + 4)
#define WM_APP_LIST_SCROLLED (WM_APP + 5)  // 列表滚动/首行变化（重排展开符号窗口）

// ---------- 消息负载 ----------
struct ProgressMsg {
    ULONGLONG items;
    ULONGLONG bytes;
    ULONGLONG skipped;
};

struct DoneMsg {
    ScanResult* result;
    ScanStats stats;
    std::wstring root;
    bool mft = false;       // 实际完成模式：true = MFT 直读
    bool failed = false;    // 扫描未完成（取消/无结果）
    std::wstring reason;
};

struct ScanJob {
    HWND hwnd;
    std::wstring root;
    std::shared_ptr<std::atomic<bool>> cancel;
    int threads;
    bool skipHidden;
    bool followReparse;
};

struct MenuJob {
    HWND hwnd;
    HMENU menu;
    std::wstring path;   // 空串 = 无文件系统路径（不加载 Shell 扩展）
    MenuConfig cfg;      // 数据驱动：菜单项开关快照
    bool useShell;       // 是否追加资源管理器扩展菜单
};

// ---------- 全局状态 ----------
HWND g_hwnd = nullptr;
HWND g_list = nullptr;
HWND g_status = nullptr;
HWND g_edit = nullptr;
HWND g_label = nullptr;
HWND g_treemap = nullptr;
HINSTANCE g_inst = nullptr;
AppSettings g_settings;
// 外观取值统一走 ThemeStore（见 theme.h/.cpp）：业务代码不硬编码任何颜色/尺寸。
static HBRUSH g_bgBrush = nullptr;   // 当前主题背景刷（切换主题时重建）

static void RebuildThemeBrushes() {
    if (g_bgBrush) DeleteObject(g_bgBrush);
    g_bgBrush = CreateSolidBrush(TC().bg);
}

HWND g_header = nullptr;      // 列表列头（自绘）

// DPI 适配：96dpi 基准缩放；S(v) 将逻辑像素换算为当前 DPI 像素
double g_dpiScale = 1.0;
HFONT g_uiFont = nullptr;     // 主界面缩放字体（创建后通过 WM_SETFONT 分发给控件）
HFONT g_glyphFont = nullptr;  // 展开符号字体（Segoe UI Symbol，含 ▸/▾ 字形）
#define S(v) ((int)((v) * g_dpiScale + 0.5))

std::unique_ptr<ScanResult> g_scan;  // 当前树（仅 UI 线程访问）
ScanNode* g_current = nullptr;       // 当前显示目录
std::vector<ScanNode*> g_view;       // 当前视图条目（指向树内节点）

bool g_scanning = false;
bool g_elevating = false;  // 提权重启中：WM_DESTROY 不等待扫描线程
static ScanEngine g_engine;      // 统一扫描引擎（MFT 优先 / 遍历降级）
ULONGLONG g_scanStart = 0;
ULONGLONG g_lastWheelTick = 0;  // 最近一次滚轮框选时间（双击误判屏蔽用）

enum class SortCol { Name, Size, Pct, Type, Files };
SortCol g_sortCol = SortCol::Size;
bool g_sortAsc = false;

ScanNode* g_menuNode = nullptr;
bool g_menuOpen = false;
HMENU g_activeMenu = nullptr;
IContextMenu* g_cm = nullptr;    // Shell 扩展菜单对象（菜单打开期间保持）
IContextMenu2* g_cm2 = nullptr;  // 用于转发 owner-draw 子菜单消息
int g_treeGen = 0;   // 树代际：每次替换树 +1（防菜单持有悬垂节点）
int g_menuGen = -1;  // 菜单打开时记录的树代际

// ---------- 状态栏 ----------
static void SetPane(int idx, const std::wstring& text) {
    if (g_status) SendMessageW(g_status, SB_SETTEXTW, idx, (LPARAM)text.c_str());
}

// ---------- 列表列定义 ----------
static void InitList() {
    const wchar_t* cols[] = { L"名称", L"大小", L"占比", L"类型", L"文件数" };
    const int widths[] = { 420, 120, 70, 90, 90 };
    for (int i = 0; i < 5; i++) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        c.pszText = const_cast<wchar_t*>(cols[i]);
        c.cx = S(widths[i]);
        c.iSubItem = i;
        ListView_InsertColumn(g_list, i, &c);
    }
    ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    ListView_SetBkColor(g_list, TC().bg);
    ListView_SetTextColor(g_list, TC().text);
    ListView_SetTextBkColor(g_list, TC().bg);
}

static std::wstring TypeOf(const ScanNode* n) {
    if (n->isDir) return L"文件夹";
    std::wstring ext = ExtOf(n->name);
    return ext.empty() ? L"文件" : ext;
}

// ---------- 排序 ----------
static int CompareNodes(const ScanNode* a, const ScanNode* b) {
    switch (g_sortCol) {
        case SortCol::Name:
            return _wcsicmp(a->name.c_str(), b->name.c_str());
        case SortCol::Size:
        case SortCol::Pct:
            return a->size < b->size ? -1 : (a->size > b->size ? 1 : 0);
        case SortCol::Files:
            return a->fileCount < b->fileCount ? -1 : (a->fileCount > b->fileCount ? 1 : 0);
        case SortCol::Type: {
            int r = _wcsicmp(TypeOf(a).c_str(), TypeOf(b).c_str());
            if (r == 0) r = _wcsicmp(a->name.c_str(), b->name.c_str());
            return r;
        }
    }
    return 0;
}

static void SortView() {
    std::stable_sort(g_view.begin(), g_view.end(), [](const ScanNode* a, const ScanNode* b) {
        int r = CompareNodes(a, b);
        return g_sortAsc ? r < 0 : r > 0;
    });
}

// ---------- 列表行模型（父级目录行始终置顶 + 目录内联展开） ----------
// ---------- 列表行模型（由 FileList 类统一管理，见 filelist.h/.cpp） ----------
// 行数据 / 展开 / 选中 / 悬浮父行 / 滚动 / 历史 / 自绘全部内聚在 FileList。
// main 侧只保留薄包装与视图排序权威（g_sortCol / g_sortAsc）。
using ListRow = FileList::Row;
static FileList g_fileList;

static bool HasParentRow() { return g_fileList.CanGoUp(); }
static void RebuildRows() { g_fileList.Rebuild(); }
static void RefreshRows() { g_fileList.Refresh(); }
static void RowHostLayout() { g_fileList.Refresh(); }
static int ListCount() { return g_fileList.Count(); }
static ListRow RowAt(int idx) { return g_fileList.RowAt(idx); }
static void SelectRowAt(int idx) { g_fileList.SelectRow(idx); }
static void ToggleExpand(ScanNode* node) { g_fileList.ToggleExpand(node); }
static bool CanExpand(const ScanNode* n) { return g_fileList.CanExpandP(n); }
static void UpdatePinState() { g_fileList.Refresh(); }
static int RowHeightPx() { return g_fileList.RowHeight(); }

// 排序后的可见子项（0 字节项保留，与树图一致）
static std::vector<ScanNode*> SortedChildren(ScanNode* dir) {
    std::vector<ScanNode*> kids;
    for (auto* c : dir->children)
        if (c) kids.push_back(c);
    std::stable_sort(kids.begin(), kids.end(), [](const ScanNode* a, const ScanNode* b) {
        int r = CompareNodes(a, b);
        return g_sortAsc ? r < 0 : r > 0;
    });
    return kids;
}


// ---------- 导航（含后退/前进历史；历史栈在 FileList 内部） ----------
static void NavigateToInternal(ScanNode* node, bool pushHistory) {
    // 根目录没有父级：任何"向上"操作都会传 nullptr 进来，
    // 不判空会解引用空指针（表现为列表被清空甚至崩溃）。
    if (!node) return;
    g_current = node;
    g_view = node->children;
    SortView();                              // main 视图排序权威
    g_fileList.NavigateTo(node, pushHistory); // FileList 内部：历史 + SetCurrent（Rebuild/Refresh）
    ListView_SetItemCountEx(g_list, g_fileList.Count(), 0);
    if (g_fileList.Count() > 0) ListView_RedrawItems(g_list, 0, g_fileList.Count() - 1);
    std::wstring full = NodeFullPath(node);
    SetWindowTextW(g_label, full.c_str());
    SetPane(0, full);
    SetPane(1, L"文件 " + FormatCount(node->fileCount) + L" / 显示 " + FormatCount(g_fileList.Count()));
    SetPane(2, L"总大小 " + FormatSize(node->size));
    InvalidateRect(g_list, nullptr, TRUE);
    RefreshRows();
    TreemapSetData(g_treemap, g_view, g_current);
}

static void NavigateTo(ScanNode* node) { NavigateToInternal(node, true); }

static void GoBack() {
    g_fileList.GoBack();
    NavigateToInternal(g_fileList.Current(), false);
}

static void GoForward() {
    g_fileList.GoForward();
    NavigateToInternal(g_fileList.Current(), false);
}

static void GoUp() {
    g_fileList.GoUp();
    NavigateToInternal(g_fileList.Current(), false);
}

// ---------- 剪贴板 / 删除 / Shell ----------
static void CopyTextToClipboard(const std::wstring& text) {
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
        wchar_t* dst = (wchar_t*)GlobalLock(h);
        if (dst) {
            memcpy(dst, text.c_str(), bytes);
            GlobalUnlock(h);
        }
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
}

static void DeleteToRecycleBin(const std::wstring& path) {
    std::wstring p = path;
    p.push_back(L'\0');  // SHFILEOPSTRUCT 要求双重空结尾
    SHFILEOPSTRUCTW op{};
    op.hwnd = g_hwnd;
    op.wFunc = FO_DELETE;
    op.pFrom = p.c_str();
    op.fFlags = FOF_ALLOWUNDO;  // 进回收站，可撤销
    if (!g_settings.confirmDelete) op.fFlags |= FOF_NOCONFIRMATION;
    SHFileOperationW(&op);
}

static std::wstring EditText() {
    int len = GetWindowTextLengthW(g_edit);
    if (len <= 0) return L"";
    std::wstring s(len, L'\0');
    GetWindowTextW(g_edit, &s[0], len + 1);
    return s;
}

// ---------- 扫描：统一引擎（ScanEngine：MFT 优先 / 遍历降级） ----------
static void StartScan(const std::wstring& rawRoot) {
    if (g_scanning) return;
    std::wstring root = NormalizeRoot(rawRoot);
    if (!IsDirPath(root)) root = L"C:\\";

    // 盘符扫描且未提权 → 自动提权重启（UAC 同意后新实例以管理员 + --autoscan 自动扫同一路径）
    bool isDrive = root.size() >= 2 && root[1] == L':' && root.size() <= 3;
    if (isDrive && !IsProcessElevated()) {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring args = std::wstring(L"--elevated --autoscan=\"") + root + L"\"";
        LogLine(L"[ELEV] 请求提权重启 root=" + root);
        if ((UINT_PTR)ShellExecuteW(nullptr, L"runas", exe, args.c_str(), nullptr,
                                    SW_SHOWNORMAL) > 32) {
            // 提权成功：本实例退出，新实例以管理员身份自动扫描
            LogLine(L"[ELEV] 提权已发起，退出当前实例");
            g_elevating = true;
            DestroyWindow(g_hwnd);
            return;
        }
        LogLine(L"[ELEV] 用户拒绝提权，以普通模式继续（普通遍历）");
    }

    // 丢弃旧树（此时无扫描线程在使用它）
    g_scan.reset();
    g_treeGen++;  // 树代际 +1：任何持有旧节点指针的菜单立即失效
    g_menuNode = nullptr;
    g_current = nullptr;
    g_view.clear();
    g_fileList.ResetScan();  // 新树：展开态/历史/行/选中/滚动全部作废
    ListView_SetItemCountEx(g_list, 0, LVSICF_NOSCROLL);
    TreemapSetData(g_treemap, {}, nullptr);
    SetWindowTextW(g_edit, root.c_str());

    g_scanning = true;
    g_scanStart = GetTickCount64();
    EnableWindow(GetDlgItem(g_hwnd, IDC_SCAN), FALSE);
    EnableWindow(GetDlgItem(g_hwnd, IDC_STOP), TRUE);
    SetPane(0, L"扫描中…  " + root);
    SetPane(1, L"0 项");
    SetPane(2, L"0 B");
    SetTimer(g_hwnd, 1, 1000, nullptr);
    LogLine(L"StartScan root=" + root + L" elev=" +
            std::to_wstring(IsProcessElevated() ? 1 : 0));

    // 引擎回调（扫描线程触发 → PostMessage 投递到 UI 线程）
    g_engine.SetCallbacks(
        [](ULONGLONG items, ULONGLONG bytes, ULONGLONG) {
            auto* m = new ProgressMsg{ items, bytes, 0 };
            if (!PostMessageW(g_hwnd, WM_APP_SCAN_PROGRESS, 0, (LPARAM)m)) delete m;
        },
        [](ScanDoneInfo&& info) {
            auto* done = new DoneMsg{ new ScanResult(std::move(*info.tree)), info.stats,
                                      info.root, info.mode == ScanMode::Mft, false, L"" };
            if (!PostMessageW(g_hwnd, WM_APP_SCAN_DONE, 0, (LPARAM)done)) delete done;
        },
        [](const std::wstring& reason) {
            auto* done = new DoneMsg{ nullptr, {}, L"", false, true, reason };
            if (!PostMessageW(g_hwnd, WM_APP_SCAN_DONE, 0, (LPARAM)done)) delete done;
        });

    // 请求模式：管理员 → MFT 直读；普通 → 遍历（引擎内自动降级）
    ScanMode mode = IsProcessElevated() ? ScanMode::Mft : ScanMode::Walk;
    if (!g_engine.Start(root, mode, g_settings)) {
        g_scanning = false;
        KillTimer(g_hwnd, 1);
        EnableWindow(GetDlgItem(g_hwnd, IDC_SCAN), TRUE);
        EnableWindow(GetDlgItem(g_hwnd, IDC_STOP), FALSE);
        SetPane(0, L"已有扫描在进行中");
    }
}

// ---------- 右键菜单：数据驱动构建 ----------
// 即时项（无 IO，主线程直接弹出）
static void AppendInstantMenuItems(HMENU menu, const MenuConfig& cfg) {
    if (cfg.open) AppendMenuW(menu, MF_STRING, IDM_OPEN, L"打开");
    if (cfg.explore) AppendMenuW(menu, MF_STRING, IDM_EXPLORE, L"在资源管理器中打开");
    if (cfg.terminal) AppendMenuW(menu, MF_STRING, IDM_TERMINAL, L"打开终端");
    if (cfg.props) AppendMenuW(menu, MF_STRING, IDM_PROPS, L"属性");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (cfg.copyPath) AppendMenuW(menu, MF_STRING, IDM_COPYPATH, L"复制完整路径");
    if (cfg.copyName) AppendMenuW(menu, MF_STRING, IDM_COPYNAME, L"复制文件名");
    if (cfg.copySize) AppendMenuW(menu, MF_STRING, IDM_COPYSIZE, L"复制大小");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, IDM_LOADING, L"正在加载更多选项...");
}

// 异步项（后台线程插入；同样受 config.json 控制）
static void AppendAsyncMenuItems(HMENU menu, const MenuConfig& cfg) {
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (cfg.enter) AppendMenuW(menu, MF_STRING, IDM_ENTER, L"进入此目录");
    if (cfg.locateList) AppendMenuW(menu, MF_STRING, IDM_TOTREE, L"在列表中定位 (Totree)");
    if (cfg.locateTreemap) AppendMenuW(menu, MF_STRING, IDM_LOCATE_TREEMAP, L"在树图中定位");
    if (cfg.selectParent) AppendMenuW(menu, MF_STRING, IDM_SELECT_PARENT, L"选择父级");
    if (cfg.goUp) AppendMenuW(menu, MF_STRING, IDM_GOUP, L"向上");
    if (cfg.back) AppendMenuW(menu, MF_STRING, IDM_BACK, L"后退");
    if (cfg.forward) AppendMenuW(menu, MF_STRING, IDM_FORWARD, L"前进");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (cfg.rescan) AppendMenuW(menu, MF_STRING, IDM_RESCAN, L"重新扫描此项");
    if (cfg.deleteRecycle) AppendMenuW(menu, MF_STRING, IDM_DELETE, L"删除到回收站...");
    if (cfg.deleteForever) AppendMenuW(menu, MF_STRING, IDM_DELETE_FOREVER, L"永久删除...");
}

// 后台菜单线程：插入异步项 + 构建资源管理器 Shell 扩展菜单（绝不阻塞 UI）
static DWORD WINAPI MenuThreadProc(LPVOID param) {
    MenuJob* j = (MenuJob*)param;
    Sleep(80);  // 留出基础菜单弹出时间

    AppendAsyncMenuItems(j->menu, j->cfg);

    IContextMenu* cm = nullptr;
    if (j->useShell && !j->path.empty()) {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(j->path.c_str(), nullptr, &pidl, 0, nullptr)) && pidl) {
            IShellFolder* parent = nullptr;
            LPCITEMIDLIST child = nullptr;
            if (SUCCEEDED(SHBindToParent(pidl, IID_PPV_ARGS(&parent), &child))) {
                if (SUCCEEDED(parent->GetUIObjectOf(nullptr, 1, &child, IID_IContextMenu,
                                                    nullptr, (void**)&cm))) {
                    DWORD added = cm->QueryContextMenu(j->menu, GetMenuItemCount(j->menu),
                                                       IDM_SHELL_FIRST, IDM_SHELL_LAST,
                                                       CMF_NORMAL);
                    if (added == (DWORD)-1) {
                        cm->Release();
                        cm = nullptr;
                    }
                }
                parent->Release();
            }
            CoTaskMemFree(pidl);
        }
        if (SUCCEEDED(hr)) CoUninitialize();
    }

    if (cm)
        PostMessageW(j->hwnd, WM_APP_SHELL_MENU_READY, (WPARAM)j->menu, (LPARAM)cm);
    else
        PostMessageW(j->hwnd, WM_APP_MENU_READY, (WPARAM)j->menu, 0);
    delete j;
    return 0;
}

// 在左侧列表中定位节点（跳转父目录并选中；深层节点自动展开其祖先链）
static void LocateInList(ScanNode* n) {
    if (!n) return;
    if (n->parent && n->parent != g_current) NavigateTo(n->parent);
    // 展开沿途祖先链（FileList 内部：ToggleExpand 自带 Rebuild）
    for (ScanNode* p = n->parent; p && p != g_current; p = p->parent) {
        if (g_fileList.CanExpandP(p) && !g_fileList.Expanded().count(p))
            g_fileList.ToggleExpand(p);
    }
    // 找到目标行并选中
    int row = -1;
    for (int i = 0; i < g_fileList.Count(); i++) {
        ListRow r = g_fileList.RowAt(i);
        if (r.node == n && !r.isParent) { row = i; break; }
    }
    if (row < 0) return;
    g_fileList.SelectRow(row);
    ListView_SetItemState(g_list, -1, 0, LVIS_SELECTED);
    ListView_SetItemState(g_list, row, LVIS_SELECTED, LVIS_SELECTED);
    ListView_EnsureVisible(g_list, row, FALSE);
}
static void HandleMenuCommand(int id) {
    if (!g_menuNode) return;
    if (g_menuGen != g_treeGen) return;  // 树已被替换：菜单基于旧树，忽略本次点击
    const std::wstring path = NodeFullPath(g_menuNode);
    switch (id) {
        case IDM_OPEN:
            ShellExecuteW(g_hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case IDM_EXPLORE: {
            // 在资源管理器中打开并选中目标（ShellExecuteExW 稳健调用）
            std::wstring args = L"/select,\"" + path + L"\"";
            SHELLEXECUTEINFOW sei{};
            sei.cbSize = sizeof(sei);
            sei.hwnd = g_hwnd;
            sei.lpVerb = L"open";
            sei.lpFile = L"explorer.exe";
            sei.lpParameters = args.c_str();
            sei.nShow = SW_SHOWNORMAL;
            ShellExecuteExW(&sei);
            break;
        }
        case IDM_COPYPATH:
            CopyTextToClipboard(path);
            break;
        case IDM_COPYNAME:
            CopyTextToClipboard(g_menuNode->name);
            break;
        case IDM_COPYSIZE:
            CopyTextToClipboard(FormatSize(g_menuNode->size));
            break;
        case IDM_TERMINAL:
            ShellExecuteW(g_hwnd, L"open", L"cmd.exe", nullptr, path.c_str(), SW_SHOWNORMAL);
            break;
        case IDM_DELETE:
            DeleteToRecycleBin(path);
            if (g_scan && g_scan->root) StartScan(NodeFullPath(g_scan->root));
            break;
        case IDM_DELETE_FOREVER: {
            std::wstring p = path;
            p.push_back(L'\0');
            // 宽字符串拼接（MinGW swprintf 的 %s 按窄串读取，不能传 wchar_t*）
            std::wstring msg = L"永久删除「" + g_menuNode->name + L"」？此操作不可恢复！";
            if (MessageBoxW(g_hwnd, msg.c_str(), L"永久删除", MB_YESNO | MB_ICONWARNING) !=
                IDYES)
                break;
            SHFILEOPSTRUCTW op{};
            op.hwnd = g_hwnd;
            op.wFunc = FO_DELETE;
            op.pFrom = p.c_str();
            op.fFlags = g_settings.confirmDelete ? 0 : FOF_NOCONFIRMATION;
            SHFileOperationW(&op);
            if (g_scan && g_scan->root) StartScan(NodeFullPath(g_scan->root));
            break;
        }
        case IDM_RENAME: {
            std::wstring name = g_menuNode->name;
            if (!ShowRenameDialog(g_hwnd, name)) break;
            std::wstring newPath = GetParentPath(path) + name;
            if (MoveFileW(path.c_str(), newPath.c_str())) {
                if (g_scan && g_scan->root) StartScan(NodeFullPath(g_scan->root));
            } else {
                SetPane(0, L"重命名失败（目标已存在或无权限）");
            }
            break;
        }
        case IDM_RESCAN:
            StartScan(path);
            break;
        case IDM_PROPS:
            ShellExecuteW(g_hwnd, L"properties", path.c_str(), nullptr, nullptr, SW_SHOW);
            break;
        case IDM_ENTER:
            if (g_menuNode->isDir)
                NavigateTo(g_menuNode);
            else
                ShellExecuteW(g_hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case IDM_TOTREE:
            LocateInList(g_menuNode);
            break;
        case IDM_LOCATE_TREEMAP:
            SendMessageW(g_treemap, WM_APP_TREEMAP_SELECT_NODE, (WPARAM)g_menuNode, 0);
            break;
        case IDM_SELECT_PARENT:
            SendMessageW(g_treemap, WM_APP_TREEMAP_SELECT_PARENT, 0, 0);
            break;
        case IDM_GOUP:
            if (g_current && g_current->parent) NavigateTo(g_current->parent);
            break;
        case IDM_BACK:
            GoBack();
            break;
        case IDM_FORWARD:
            GoForward();
            break;
    }
}

static void ShowContextMenuFor(ScanNode* node, POINT pt) {
    if (!node) return;
    g_menuNode = node;
    g_menuGen = g_treeGen;  // 记录菜单对应的树代际（树替换后点击自动失效）

    // 第一步：瞬间弹出基础菜单（全部为无 IO 的即时操作，数据驱动）
    HMENU menu = CreatePopupMenu();
    AppendInstantMenuItems(menu, g_settings.menu);

    // 第二步：后台线程加载异步项 + 资源管理器扩展菜单，完成后异步接管
    g_activeMenu = menu;
    g_menuOpen = true;
    auto* mj = new MenuJob{ g_hwnd, menu, NodeFullPath(node), g_settings.menu,
                            g_settings.menu.shellMenu };
    HANDLE ht = CreateThread(nullptr, 0, MenuThreadProc, mj, 0, nullptr);
    if (ht)
        CloseHandle(ht);
    else
        delete mj;

    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
    g_menuOpen = false;
    g_activeMenu = nullptr;
    if (g_cm) {
        g_cm->Release();
        g_cm = nullptr;
    }
    if (g_cm2) {
        g_cm2->Release();
        g_cm2 = nullptr;
    }
    DestroyMenu(menu);
}

// 现代化扁平按钮（自绘：圆角石墨底色 + 浅色文字）
// 现代化扁平按钮：无边框、圆角、主操作使用强调色，次操作用半透明表面色
// 通过控件 ID 区分主次：扫描/确定=主色，其余=次色
static void DrawFlatButton(HDC dc, const RECT* rc, const wchar_t* text, UINT state, int id) {
    bool disabled = (state & ODS_DISABLED) != 0;
    bool pressed = (state & ODS_SELECTED) != 0;
    bool hover = (state & ODS_HOTLIGHT) != 0;
    bool primary = (id == IDC_SCAN || id == 220 /*B_OK*/);
    bool danger = (id == IDC_STOP);
    COLORREF bg;
    if (primary) bg = disabled ? RGB(52, 62, 78) : pressed ? TC().accentPressed : TC().accent;
    else if (danger) bg = disabled ? TC().button : pressed ? TC().dangerPressed : TC().danger;
    else bg = disabled ? TC().button : pressed ? TC().buttonPressed : hover ? TC().buttonHover : TC().button;
    RECT r = *rc;
    // 圆角填充（无边框：现代化做法）
    HRGN rgn = CreateRoundRectRgn(r.left, r.top, r.right + 1, r.bottom + 1, S(8), S(8));
    HBRUSH br = CreateSolidBrush(bg);
    FillRgn(dc, rgn, br);
    DeleteObject(rgn);
    DeleteObject(br);
    HFONT of = (HFONT)SelectObject(dc, g_uiFont);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? TC().textDim
                              : (primary || danger ? TC().onAccent : TC().text));
    RECT tr = r;
    tr.left += S(4);
    tr.right -= S(4);
    DrawTextW(dc, text, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, of);
}



// 把当前主题应用到所有控件（切换主题时调用）。
// 关键点：
//   1. 状态栏文字色必须用 SB_SETTEXTCOLOR 单独设置，只设背景色会导致浅色主题下
//      文字仍是深色、落在深色背景上完全看不见。
//   2. 注册过的窗口类背景刷改 SetClassLongPtr 后必须 RedrawWindow(RDW_ERASE)
//      才会生效，否则窗口底仍是旧主题的颜色。
//   3. 编辑框/按钮是子控件，必须整棵子树重绘，否则切主题后它们保持旧配色。
static void UpdateThemeButton();

static void ApplyTheme() {
    RebuildThemeBrushes();
    const ThemeColors& t = TC();
    // 窗口类背景刷（自绘控件以外的默认擦除用）
    SetClassLongPtrW(g_hwnd, GCLP_HBRBACKGROUND, (LONG_PTR)g_bgBrush);
    g_fileList.ApplyTheme();  // 列表/行窗口/「..」行全套
    // 状态栏：背景 + 文字色都要设
    if (g_status) {
        SendMessageW(g_status, SB_SETBKCOLOR, 0, (LPARAM)t.panel);
        SendMessageW(g_status, SB_SETTEXTCOLOR, 0, (LPARAM)t.text);
    }
    // 整棵子树强制重绘（含擦除），让背景刷/CTLCOLOR/自绘全部按新主题走
    RedrawWindow(g_hwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    if (g_status) InvalidateRect(g_status, nullptr, TRUE);
    if (g_treemap) TreemapSetSettings(g_treemap, g_settings);
    RedrawWindow(g_hwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    UpdateThemeButton();
}

static void UpdateThemeButton() {
    HWND b = GetDlgItem(g_hwnd, IDC_THEME);
    if (b) SetWindowTextW(b, g_settings.theme == 1 ? L"浅色" : L"深色");
}

// 面积算法控件同步（combo 选中 + 幂滑杆可见性/位置，与 g_settings 双向绑定）
static void SyncWeightControls() {
    HWND hMap = GetDlgItem(g_hwnd, IDC_MAP);
    if (hMap) SendMessageW(hMap, CB_SETCURSEL, g_settings.mapKind, 0);
    HWND hOrd = GetDlgItem(g_hwnd, IDC_ORD);
    if (hOrd) SendMessageW(hOrd, CB_SETCURSEL, g_settings.ordKind, 0);
    HWND hPow = GetDlgItem(g_hwnd, IDC_POWA);
    if (hPow) {
        ShowWindow(hPow, g_settings.mapKind == 4 ? SW_SHOW : SW_HIDE);
        SendMessageW(hPow, TBM_SETPOS, TRUE,
                     (LPARAM)(int)(g_settings.powAlpha * 100 + 0.5));
    }
}

// 面积算法变化 → 存设置 + 树图重算
static void ApplyWeightSettings() {
    SaveSettings(g_settings);
    if (g_treemap) {
        TreemapSetSettings(g_treemap, g_settings);
        InvalidateRect(g_treemap, nullptr, FALSE);
    }
}

// ---------- 窗口过程 ----------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            // g_hwnd 必须在任何使用它的调用之前赋值：
            // ApplyTheme()/SetClassLongPtrW()/RedrawWindow() 都以 g_hwnd 为目标，
            // 之前把它放在 WM_CREATE 末尾，导致这些调用全部作用在 nullptr 上。
            g_hwnd = hwnd;
            HDC sdc = GetDC(hwnd);
            g_dpiScale = GetDeviceCaps(sdc, LOGPIXELSX) / 96.0;
            ReleaseDC(hwnd, sdc);
            if (g_dpiScale < 1.0) g_dpiScale = 1.0;
            g_uiFont = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                   CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
            // 展开符号专用字体：▸(U+25B8)/▾(U+25BE) 在微软雅黑里没有字形，
            // 直接用雅黑会画成空白/方块。Segoe UI Symbol 是 Windows 必装且含这两个字形。
            g_glyphFont = CreateFontW(-S(11), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0,
                                      0, CLEARTYPE_QUALITY, 0, L"Segoe UI Symbol");
            // 所有子控件统一使用缩放字体（树图自带字体，会忽略 WM_SETFONT 后的默认绘制）
            auto applyFont = [&](HWND h, LPARAM f) { SendMessageW(h, WM_SETFONT, f, TRUE); };
            auto makeBtn = [&](int id, const wchar_t* text, int x, int w) {
                HWND h = CreateWindowExW(0, L"BUTTON", text,
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON |
                                             BS_OWNERDRAW,
                                         S(x), S(11), S(w), S(26), hwnd, (HMENU)(INT_PTR)id, g_inst,
                                         nullptr);
                applyFont(h, (LPARAM)g_uiFont);
                return h;
            };
            makeBtn(IDC_BACK, L"← 返回", 8, 58);
            makeBtn(IDC_UP, L"↑ 上级", 70, 58);
            g_edit = CreateWindowExW(0, L"EDIT", L"C:\\",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                     S(132), S(11), S(240), S(26), hwnd, (HMENU)IDC_PATH_EDIT, g_inst,
                                     nullptr);
            applyFont(g_edit, (LPARAM)g_uiFont);
            makeBtn(IDC_BROWSE, L"浏览", 376, 54);
            makeBtn(IDC_SCAN, L"扫描", 434, 66);
            makeBtn(IDC_STOP, L"停止", 504, 58);
            // 面积算法：映射 g + 顺序 + 幂指数 α（与 Web 版工具栏一致）
            auto makeCombo = [&](int id, int x, int w) {
                HWND h = CreateWindowExW(0, L"COMBOBOX", L"",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                             CBS_DROPDOWNLIST | WS_VSCROLL,
                                         S(x), S(11), S(w), S(220), hwnd, (HMENU)(INT_PTR)id,
                                         g_inst, nullptr);
                applyFont(h, (LPARAM)g_uiFont);
                return h;
            };
            HWND hMap = makeCombo(IDC_MAP, 566, 104);
            SendMessageW(hMap, CB_ADDSTRING, 0, (LPARAM)L"映射 g=x");
            SendMessageW(hMap, CB_ADDSTRING, 0, (LPARAM)L"映射 g=log₂x");
            SendMessageW(hMap, CB_ADDSTRING, 0, (LPARAM)L"映射 g=log₂²x");
            SendMessageW(hMap, CB_ADDSTRING, 0, (LPARAM)L"映射 g=√x");
            SendMessageW(hMap, CB_ADDSTRING, 0, (LPARAM)L"映射 g=x^α");
            HWND hOrd = makeCombo(IDC_ORD, 674, 118);
            SendMessageW(hOrd, CB_ADDSTRING, 0, (LPARAM)L"先和→g(Σx)");
            SendMessageW(hOrd, CB_ADDSTRING, 0, (LPARAM)L"先 g→Σg(x)");
            SendMessageW(hOrd, CB_ADDSTRING, 0, (LPARAM)L"g(Σg(x))");
            HWND hPow = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                                        WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                                        S(788), S(12), S(88), S(22), hwnd,
                                        (HMENU)(INT_PTR)IDC_POWA, g_inst, nullptr);
            SendMessageW(hPow, TBM_SETRANGE, TRUE, MAKELPARAM(10, 100));
            applyFont(hPow, (LPARAM)g_uiFont);
            makeBtn(IDC_THEME, L"深色", 880, 58);
            makeBtn(IDC_REFRESH, L"刷新", 942, 66);
            makeBtn(IDC_SETTINGS, L"设置", 1008, 58);
            EnableWindow(GetDlgItem(hwnd, IDC_STOP), FALSE);

            g_label = CreateWindowExW(0, L"STATIC", L"就绪",
                                      WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
                                      S(8), S(46), S(800), S(18), hwnd, (HMENU)IDC_PATH_LABEL,
                                      g_inst, nullptr);
            applyFont(g_label, (LPARAM)g_uiFont);

            // 左侧文件列表：全部由 FileList 类管理（行/展开/选中/悬浮/滚动/自绘）
            g_fileList.Create(hwnd, g_inst, &g_settings);
            g_list = g_fileList.ListHwnd();
            g_header = ListView_GetHeader(g_list);
            applyFont(g_header, (LPARAM)g_uiFont);
            g_fileList.SetCallbacks({
                nullptr,  // onEnterDir（双击进入已由 FileList 内部处理）
                [](ScanNode* n) {  // onOpenFile：双击文件
                    if (!n) return;
                    ShellExecuteW(g_hwnd, L"open", NodeFullPath(n).c_str(), nullptr,
                                  nullptr, SW_SHOWNORMAL);
                },
                [](ScanNode* n, POINT pt) {  // onContext：右键菜单
                    if (n) ShowContextMenuFor(n, pt);
                },
                [](ScanNode* n) {  // onSelect：选中联动树图
                    if (n)
                        SendMessageW(g_treemap, WM_APP_TREEMAP_SELECT_NODE, (WPARAM)n, 0);
                },
            });

            // 路径框左侧的「路径」标签（现代化扁平的字段前置说明）
            HWND pathCap = CreateWindowExW(0, L"STATIC", L"路径",
                                           WS_CHILD | WS_VISIBLE | SS_LEFT,
                                           S(12), S(15), S(88), S(20), hwnd, nullptr, g_inst,
                                           nullptr);
            applyFont(pathCap, (LPARAM)g_uiFont);
            g_status = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
                                       WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, hwnd,
                                       (HMENU)IDC_STATUS, g_inst, nullptr);
            applyFont(g_status, (LPARAM)g_uiFont);
            int parts[] = { S(360), S(540), S(720), -1 };
            SendMessageW(g_status, SB_SETPARTS, 4, (LPARAM)parts);
            SetPane(0, L"就绪：输入路径后点击「扫描」（默认 C:\\）");

            g_treemap = TreemapCreate(hwnd, g_inst);
            LoadSettings(g_settings);  // 从 config.json 双向绑定加载（含默认值）
            // 主题以共享 config.json 的 ui.theme 为准（与按钮状态一致），同步 theme.json preset
            ThemeStore::Instance().SelectPreset(g_settings.theme == 1 ? 1 : 0);
            TreemapSetSettings(g_treemap, g_settings);
            SyncWeightControls();
            ApplyTheme();  // 启动即按配置应用主题（含状态栏文字色）

            std::wstring lp = g_settings.lastPath;
            if (lp.empty() || !IsDirPath(lp)) lp = L"C:\\";
            SetWindowTextW(g_edit, lp.c_str());
            return 0;
        }

        case WM_SIZE: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            int w = rc.right, h = rc.bottom;
            MoveWindow(g_status, 0, h - S(24), w, S(24), TRUE);
            int availH = h - S(24) - S(70) - S(8);
            if (availH < S(50)) availH = S(50);
            MoveWindow(g_edit, S(104), S(11), S(300), S(26), TRUE);
            MoveWindow(g_label, S(8), S(46), w - S(16), S(18), TRUE);
            int split = w * 55 / 100;  // 左列表 / 右树图
            int upH = g_fileList.RowHeight();  // 「..」行与行窗口同高，保证垂直对齐
            // 表头高度：行容器必须从表头下方开始，否则盖住列头（「顶部大小列自动
            // 隐藏」的根因），且行内容与列头垂直错位（「箭头与文字错位」的根因）
            int headerH = 0;
            if (g_header) {
                RECT hrc{};
                GetWindowRect(g_header, &hrc);
                headerH = hrc.bottom - hrc.top;
                if (headerH <= 0) headerH = S(24);
            }
            int listTop = S(70) + upH + headerH;
            // FileList 内部统一摆放 列表/行容器（避开滚动条）/「..」行，从表头下方开始
            g_fileList.Layout(S(8), listTop, split - S(16), availH - upH - headerH, upH);
            MoveWindow(g_treemap, split + S(8), S(70), w - split - S(16), availH, TRUE);
            return 0;
        }

        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            mmi->ptMinTrackSize.x = S(820);
            mmi->ptMinTrackSize.y = S(340);
            return 0;
        }

        case WM_TIMER: {
            ULONGLONG sec = (GetTickCount64() - g_scanStart) / 1000;
            wchar_t buf[64];
            swprintf(buf, 64, L"用时 %llu 秒", sec);
            SetPane(3, buf);
            return 0;
        }

        // Shell 扩展菜单需要转发这三类消息才能正确绘制子菜单
        case WM_INITMENUPOPUP:
            if (g_cm2 && SUCCEEDED(g_cm2->HandleMenuMsg(msg, wParam, lParam))) return 0;
            break;
        // 深色主题：统一背景与文字色
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLORLISTBOX: {
            HDC dc = (HDC)wParam;
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, TC().text);
            SetBkColor(dc, TC().bg);
            return (LRESULT)(g_bgBrush ? g_bgBrush : (HBRUSH)GetStockObject(BLACK_BRUSH));
        }

        case WM_DRAWITEM: {
            DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lParam;
            if (!dis) break;
            if (dis->CtlType == ODT_MENU) {
                // Shell 扩展菜单的 owner-draw 项
                if (g_cm2) {
                    g_cm2->HandleMenuMsg(msg, wParam, lParam);
                    return TRUE;
                }
                break;
            }
            if (dis->CtlType == ODT_BUTTON) {
                wchar_t txt[64];
                GetWindowTextW(dis->hwndItem, txt, 64);
                DrawFlatButton(dis->hDC, &dis->rcItem, txt, dis->itemState,
                               (int)dis->CtlID);
                return TRUE;
            }
            break;
        }
        case WM_MEASUREITEM:
            if (g_cm2) {
                g_cm2->HandleMenuMsg(msg, wParam, lParam);
                return TRUE;
            }
            break;

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            // 资源管理器扩展命令：转发给 Shell InvokeCommand
            if (id >= IDM_SHELL_FIRST && id <= IDM_SHELL_LAST && g_cm) {
                CMINVOKECOMMANDINFOEX ci{};
                ci.cbSize = sizeof(ci);
                ci.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
                ci.hwnd = hwnd;
                ci.lpVerbW = MAKEINTRESOURCEW(id - IDM_SHELL_FIRST);
                GetCursorPos(&ci.ptInvoke);
                ci.nShow = SW_SHOWNORMAL;
                g_cm->InvokeCommand((LPCMINVOKECOMMANDINFO)&ci);
                return 0;
            }
            if (id >= IDM_OPEN && id <= IDM_LOCATE_TREEMAP) {
                HandleMenuCommand(id);
                return 0;
            }
            switch (id) {
                case IDC_SCAN:
                    StartScan(EditText());
                    return 0;
                case IDC_STOP:
                    g_engine.Cancel();
                    return 0;
                case IDC_BACK:
                    GoBack();
                    return 0;
                    return 0;
                case IDC_UP:
                    GoUp();
                    return 0;
                case IDC_MAP:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        LRESULT sel = SendMessageW(GetDlgItem(hwnd, IDC_MAP),
                                                   CB_GETCURSEL, 0, 0);
                        if (sel != CB_ERR) { g_settings.mapKind = (int)sel; ApplyWeightSettings(); }
                        HWND hPow = GetDlgItem(hwnd, IDC_POWA);
                        if (hPow) ShowWindow(hPow, g_settings.mapKind == 4 ? SW_SHOW : SW_HIDE);
                    }
                    return 0;
                case IDC_ORD:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        LRESULT sel = SendMessageW(GetDlgItem(hwnd, IDC_ORD),
                                                   CB_GETCURSEL, 0, 0);
                        if (sel != CB_ERR) { g_settings.ordKind = (int)sel; ApplyWeightSettings(); }
                    }
                    return 0;
                case IDC_REFRESH:
                    if (g_scan && g_scan->root) StartScan(NodeFullPath(g_scan->root));
                    return 0;
                case IDC_BROWSE: {
                    BROWSEINFOW bi{};
                    bi.hwndOwner = hwnd;
                    bi.lpszTitle = L"选择要扫描的文件夹或磁盘";
                    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
                    if (pidl) {
                        wchar_t path[MAX_PATH];
                        if (SHGetPathFromIDListW(pidl, path)) SetWindowTextW(g_edit, path);
                        CoTaskMemFree(pidl);
                    }
                    return 0;
                }
                case IDC_THEME: {
                    // 工具栏直接切换主题（与 Web 版一致）
                    g_settings.theme = g_settings.theme == 1 ? 0 : 1;
                    ThemeStore::Instance().SelectPreset(g_settings.theme);
                    SaveSettings(g_settings);
                    ApplyTheme();
                    if (g_treemap) TreemapSetSettings(g_treemap, g_settings);
                    return 0;
                }
                case IDC_SETTINGS:
                    if (ShowSettingsDialog(hwnd, g_settings)) {
                        // 外观层：主题写 theme.json（唯一真源），立即生效
                        ThemeStore::Instance().SelectPreset(g_settings.theme == 1 ? 1 : 0);
                        ApplyTheme();
                        TreemapSetSettings(g_treemap, g_settings);  // 树图立即生效
                        if (g_current) {
                            // 默认排序立即生效
                            g_sortCol = g_settings.defaultSort == 1 ? SortCol::Name
                                                                    : SortCol::Size;
                            g_sortAsc = g_settings.defaultSort == 1;
                            SortView();
                            RebuildRows();
                            RefreshRows();
                            SendMessageW(hwnd, WM_SIZE, 0, 0);
                        }
                    }
                    return 0;
            }
            break;
        }

        case WM_CONTEXTMENU: {
            POINT pt;
            if (lParam == (LPARAM)-1) GetCursorPos(&pt);
            else {
                pt.x = GET_X_LPARAM(lParam);
                pt.y = GET_Y_LPARAM(lParam);
            }
            ScanNode* node = nullptr;
            if (g_current) {
                POINT client = pt;
                ScreenToClient(g_list, &client);
                LVHITTESTINFO hit{};
                hit.pt = client;
                int idx = ListView_HitTest(g_list, &hit);
                if (idx >= 0 && idx < ListCount()) {
                    ListView_SetItemState(g_list, idx, LVIS_SELECTED, LVIS_SELECTED);
                    node = RowAt(idx).node;
                } else {
                    node = g_current;
                }
            }
            ShowContextMenuFor(node, pt);
            return 0;
        }

        case WM_APP_TREEMAP_NAV: {
            ScanNode* n = (ScanNode*)lParam;
            if (!n) break;
            // 上滚框选后 500ms 内不进入（列表/「..」行/树图的误判双击都会走到这里）
            if (GetTickCount64() - g_lastWheelTick < 500) {
                SendMessageW(g_treemap, WM_APP_TREEMAP_SELECT_NODE, (WPARAM)n, 0);
                return 0;
            }
            if (n->isDir)
                NavigateTo(n);
            else
                ShellExecuteW(hwnd, L"open", NodeFullPath(n).c_str(), nullptr, nullptr,
                              SW_SHOWNORMAL);
            return 0;
        }

        case WM_APP_TREEMAP_LOCATE: {  // 双向绑定：树图单击 → 定位到左侧列表
            LocateInList((ScanNode*)wParam);
            return 0;
        }

        case WM_MOUSEWHEEL: {
            // 鼠标悬浮在树图区域时滚轮交给树图（上滚=选父级，下滚=进最大子项）；
            // 其余区域走默认（列表滚动等）
            POINT pt;
            GetCursorPos(&pt);
            RECT rc;
            if (g_treemap && GetWindowRect(g_treemap, &rc) && PtInRect(&rc, pt)) {
                g_lastWheelTick = GetTickCount64();  // 滚轮框选刚发生：屏蔽随后的双击误判
                return SendMessageW(g_treemap, WM_MOUSEWHEEL, wParam, lParam);
            }
            break;
        }

        // 列表滚动 / 首行变化：更新悬浮固定态
        case WM_APP_LIST_SCROLLED: {
            g_fileList.OnListScroll(g_list ? ListView_GetTopIndex(g_list) : 0);
            return 0;
        }

        case WM_VSCROLL:
        case WM_HSCROLL: {
            // 幂指数滑杆（mapKind==4）
            if ((HWND)lParam == GetDlgItem(hwnd, IDC_POWA)) {
                LRESULT pos = SendMessageW((HWND)lParam, TBM_GETPOS, 0, 0);
                g_settings.powAlpha = pos / 100.0;
                ApplyWeightSettings();
                return 0;
            }
            LRESULT r = DefWindowProcW(hwnd, msg, wParam, lParam);
            if ((HWND)lParam == g_list)
                g_fileList.OnListScroll(ListView_GetTopIndex(g_list));
            return r;
        }

        case WM_APP_TREEMAP_MENU: {
            ScanNode* n = (ScanNode*)wParam;
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ShowContextMenuFor(n, pt);
            return 0;
        }

        case WM_APP_MENU_READY: {
            // 菜单仍打开时才操作（TrackPopupMenu 运行期间窗口仍处理消息）
            if (!g_menuOpen || (HMENU)wParam != g_activeMenu) return 0;
            HMENU menu = (HMENU)wParam;
            int cnt = GetMenuItemCount(menu);
            for (int i = 0; i < cnt; i++) {
                if (GetMenuItemID(menu, i) == IDM_LOADING) {
                    RemoveMenu(menu, i, MF_BYPOSITION);
                    break;
                }
            }
            return 0;
        }

        case WM_APP_SHELL_MENU_READY: {
            if (!g_menuOpen || (HMENU)wParam != g_activeMenu) {
                ((IContextMenu*)lParam)->Release();  // 菜单已关闭，释放 Shell 对象
                return 0;
            }
            HMENU menu = (HMENU)wParam;
            int cnt = GetMenuItemCount(menu);
            for (int i = 0; i < cnt; i++) {
                if (GetMenuItemID(menu, i) == IDM_LOADING) {
                    RemoveMenu(menu, i, MF_BYPOSITION);
                    break;
                }
            }
            g_cm = (IContextMenu*)lParam;
            g_cm->QueryInterface(IID_IContextMenu2, (void**)&g_cm2);
            return 0;
        }

        case WM_APP_SCAN_PROGRESS: {
            ProgressMsg* m = (ProgressMsg*)lParam;
            if (m) {
                SetPane(1, L"条目 " + FormatCount(m->items));
                SetPane(2, L"大小 " + FormatSize(m->bytes));
                delete m;
            }
            return 0;
        }

        case WM_APP_SCAN_DONE: {
            DoneMsg* done = (DoneMsg*)lParam;
            LogLine(L"SCAN DONE failed=" + std::to_wstring(done->failed ? 1 : 0) +
                    L" mft=" + std::to_wstring(done->mft ? 1 : 0) +
                    L" items=" + std::to_wstring(done->stats.items) +
                    L" bytes=" + std::to_wstring(done->stats.bytes) +
                    L" root=" + done->root);
            g_scan.reset(done->result);
            g_treeGen++;  // 树代际 +1：新树接管，旧节点指针全部失效
            g_scanning = false;
            KillTimer(hwnd, 1);
            EnableWindow(GetDlgItem(hwnd, IDC_SCAN), TRUE);
            EnableWindow(GetDlgItem(hwnd, IDC_STOP), FALSE);
            if (done->failed) {
                SetPane(0, L"扫描失败：" + done->reason);
                SetPane(1, L"");
                SetPane(2, L"");
                SetPane(3, L"");
                delete done;
                return 0;
            }
            const wchar_t* mode = done->mft ? L"MFT 直读" : L"普通遍历";
            if (done->stats.cancelled)
                SetPane(0, std::wstring(mode) + L" 已取消（显示部分结果）  " + done->root);
            else if (done->stats.skipped)
                SetPane(0, std::wstring(mode) + L" 完成（跳过 " +
                               FormatCount(done->stats.skipped) + L" 项）  " + done->root);
            else
                SetPane(0, std::wstring(mode) + L" 完成  " + done->root);
            ULONGLONG sec = (GetTickCount64() - g_scanStart) / 1000;
            wchar_t buf[64];
            swprintf(buf, 64, L"用时 %llu 秒", sec);
            SetPane(3, buf);
            // 应用默认排序（设置项）
            g_sortCol = g_settings.defaultSort == 1 ? SortCol::Name : SortCol::Size;
            g_sortAsc = g_settings.defaultSort == 1;
            // 记住上次扫描路径（双向绑定回写）
            g_settings.lastPath = done->root;
            SaveSettings(g_settings);
            LogLine(L"navigate start root=" + std::to_wstring(g_scan->root != nullptr));
            NavigateTo(g_scan->root);
            LogLine(L"navigate done rows=" + std::to_wstring(g_fileList.Count()));
            // 诊断：把行模型状态报进状态栏（验证 hasChildren/depth/箭头位是否成立）
            {
                int dirs = 0, files = 0, withKids = 0;
                for (int ri = 0; ri < g_fileList.Count(); ri++) {
                    ListRow r = g_fileList.RowAt(ri);
                    if (r.isParent) { withKids += r.hasChildren ? 1 : 0; continue; }
                    if (r.node && r.node->isDir) dirs++;
                    else files++;
                    if (r.hasChildren) withKids++;
                }
                wchar_t dbg[256];
                swprintf(dbg, 256,
                         L"[DIAG] 行%zu 目录%d 文件%d 可展开%d | arrowBox=%d indent=%d padX=%d | 首行depth=%d hasKids=%d",
                         (size_t)g_fileList.Count(), dirs, files, withKids, TM().arrowBox, TM().indent,
                         TM().padX, g_fileList.Count() > 0 ? g_fileList.RowAt(0).depth : -1,
                         g_fileList.Count() > 0 ? (g_fileList.RowAt(0).hasChildren ? 1 : 0) : -1);
                SetPane(2, dbg);
            }
            return 0;
        }

        case WM_NOTIFY: {
            NMHDR* hdr = (NMHDR*)lParam;
            // 列头自绘（深色）
            if (g_header && hdr->hwndFrom == g_header && hdr->code == NM_CUSTOMDRAW) {
                NMCUSTOMDRAW* cd = (NMCUSTOMDRAW*)lParam;
                if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                    HDC hdc = cd->hdc;
                    RECT rc = cd->rc;
                    HBRUSH b = CreateSolidBrush(TC().panel);
                    FillRect(hdc, &rc, b);
                    DeleteObject(b);
                    HPEN p = CreatePen(PS_SOLID, 1, TC().line);
                    HPEN op = (HPEN)SelectObject(hdc, p);
                    MoveToEx(hdc, rc.right - 1, rc.top, nullptr);
                    LineTo(hdc, rc.right - 1, rc.bottom);
                    SelectObject(hdc, op);
                    DeleteObject(p);
                    wchar_t buf[64];
                    LVCOLUMNW col{};
                    col.mask = LVCF_TEXT;
                    col.pszText = buf;
                    col.cchTextMax = 64;
                    ListView_GetColumn(g_list, (int)cd->dwItemSpec, &col);
                    SetBkMode(hdc, TRANSPARENT);
                    SetTextColor(hdc, TC().textDim);
                    RECT tr = rc;
                    tr.left += 7;
                    DrawTextW(hdc, buf, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                    return CDRF_SKIPDEFAULT;
                }
                return CDRF_DODEFAULT;
            }
            if (hdr->hwndFrom != g_list) break;
            switch (hdr->code) {
                case LVN_GETDISPINFOW: {
                    NMLVDISPINFOW* di = (NMLVDISPINFOW*)lParam;
                    int idx = (int)di->item.iItem;
                    ListRow row = RowAt(idx);
                    const ScanNode* n = row.node;
                    // 关键：无论行数据是否有效，都必须给 pszText 写入内容。
                    // 之前这里直接 break，等于「该行不提供文本」，列表就画成空白——
                    // 这正是「名称栏全空」的根因（g_rows 为空/行越界时命中）。
                    if (!n) {
                        if (di->item.mask & LVIF_TEXT) di->item.pszText[0] = L'\0';
                        break;
                    }
                    const bool pinned = (idx == 0 && !row.isParent);
                    const std::wstring displayName =
                        pinned ? g_fileList.PinLabel() : (row.isParent ? L".." : n->name);
                    if (!(di->item.mask & LVIF_TEXT)) break;
                    wchar_t buf[160];
                    switch (di->item.iSubItem) {
                        case 0: {
                            // 行内容已由行窗口层（RowItemProc）绘制，这里只给纯名称，
                            // 不再做缩进/箭头（避免两套绘制互相覆盖）。
                            lstrcpynW(di->item.pszText, displayName.c_str(), di->item.cchTextMax);
                            break;
                        }
                        case 1: {
                            std::wstring s = FormatSize(n->size);
                            lstrcpynW(di->item.pszText, s.c_str(), di->item.cchTextMax);
                            break;
                        }
                        case 2: {
                            // 父级行：当前文件夹占其父级的比例；子行：占当前文件夹的比例
                            ULONGLONG total = 0;
                            if (row.isParent) {
                                if (n->parent) total = n->parent->size;
                            } else if (g_current) {
                                total = g_current->size;
                            }
                            if (total) {
                                double pct = (double)n->size / (double)total * 100.0;
                                swprintf(buf, 160, L"%.1f%%", pct);
                            } else {
                                buf[0] = L'\0';
                            }
                            lstrcpynW(di->item.pszText, buf, di->item.cchTextMax);
                            break;
                        }
                        case 3: {
                            std::wstring t = row.isParent ? L"上级目录" : TypeOf(n);
                            lstrcpynW(di->item.pszText, t.c_str(), di->item.cchTextMax);
                            break;
                        }
                        case 4: {
                            std::wstring s = FormatCount(n->fileCount);
                            lstrcpynW(di->item.pszText, s.c_str(), di->item.cchTextMax);
                            break;
                        }
                    }
                    break;
                }
                case NM_CUSTOMDRAW: {
                    // 行内容全部由行窗口层绘制；列表本体不画任何行
                    // （否则会和行层叠加出重影）。
                    NMLVCUSTOMDRAW* cd = (NMLVCUSTOMDRAW*)lParam;
                    if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) {
                        HDC dc = cd->nmcd.hdc;
                        RECT rc = cd->nmcd.rc;
                        HBRUSH bg = CreateSolidBrush(TC().bg);
                        FillRect(dc, &rc, bg);
                        DeleteObject(bg);
                        return CDRF_SKIPDEFAULT;
                    }
                    return CDRF_DODEFAULT;
                }
                case LVN_ODSTATECHANGED: {  // 虚拟列表首行变化（滚动）：同步行层
                    PostMessageW(hwnd, WM_APP_LIST_SCROLLED, 0, 0);
                    break;
                }
                case LVN_COLUMNCLICK: {
                    NMLISTVIEW* nm = (NMLISTVIEW*)lParam;
                    SortCol col = SortCol::Name;
                    switch (nm->iSubItem) {
                        case 0: col = SortCol::Name; break;
                        case 1: col = SortCol::Size; break;
                        case 2: col = SortCol::Pct; break;
                        case 3: col = SortCol::Type; break;
                        case 4: col = SortCol::Files; break;
                    }
                    if (col == g_sortCol) g_sortAsc = !g_sortAsc;
                    else { g_sortCol = col; g_sortAsc = (col == SortCol::Name); }
                    SortView();
                    g_fileList.SetSort((int)col, g_sortAsc);  // 同步 FileList 内部排序/行重建
                    return 0;
                }
            }
            break;
        }


        case WM_DESTROY: {
            if (!g_elevating) {
                g_engine.Cancel();
                while (g_engine.Running()) Sleep(50);   // 等扫描线程收尾（引擎线程 detach，只等标志）
                Sleep(100);
            }
            g_scan.reset();
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR pCmdLine, int nCmdShow) {
    LogLine(L"== native start args=" + std::wstring(pCmdLine ? pCmdLine : L""));
    // DPI 感知：声明必须在任何窗口创建之前（优先 Per-Monitor V2，退回 System DPI）
    {
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        typedef BOOL (WINAPI *SetCtxFn)(void*);
        auto setCtx = u32 ? (SetCtxFn)GetProcAddress(u32, "SetProcessDpiAwarenessContext") : nullptr;
        BOOL ok = FALSE;
        if (setCtx) ok = setCtx((void*)-4);   // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        if (!ok) {
            typedef BOOL (WINAPI *SetAwareFn)(int);
            auto setAware = u32 ? (SetAwareFn)GetProcAddress(u32, "SetProcessDPIAware") : nullptr;
            if (setAware) setAware(1);
        }
    }
    // 提权：--elevated 且非管理员 → ShellExecuteW(RunAs) 以管理员重启
    std::wstring args = pCmdLine ? pCmdLine : L"";
    if (args.find(L"--elevated") != std::wstring::npos && !IsProcessElevated()) {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if ((UINT_PTR)ShellExecuteW(nullptr, L"runas", exe, args.c_str(), nullptr,
                                    SW_SHOWNORMAL) > 32)
            return 0;   // 新实例接管；本实例退出
        // RunAs 被拒 → 继续普通模式运行
    }
    // --autoscan=<路径>：启动后自动扫描该目录
    std::wstring autoScan;
    {
        size_t ap = args.find(L"--autoscan=");
        if (ap != std::wstring::npos) {
            std::wstring v = args.substr(ap + 11);
            size_t sp = v.find(L' ');
            if (sp != std::wstring::npos) v = v.substr(0, sp);
            // 去掉命令行引号（提权重启时路径带 " 包裹）
            if (v.size() >= 2 && v.front() == L'"' && v.back() == L'"')
                v = v.substr(1, v.size() - 2);
            autoScan = v;
        }
    }
    int winW = 1080, winH = 560;
    g_inst = hInstance;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // Shell 扩展 InvokeCommand 需要

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_WIN95_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    // 外观层：主题的唯一真源是 theme.json（`theme.json` 的 preset 字段）。
    // 不从 config.json 读 theme —— 避免两处配置互相覆盖导致切换失效。
    // Load 内部：首次读文件里的 preset；文件缺失则写一份默认预设。
    ThemeStore::Instance().Load(L"");
    RebuildThemeBrushes();

    TreemapRegister(hInstance);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = g_bgBrush;
    wc.lpszClassName = L"DiskMateWnd";
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hIconSm = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    // 窗口初始尺寸按当前 DPI 缩放（否则高 DPI 下工具栏按钮溢出窗口）
    {
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        typedef UINT (WINAPI *GetDpiForSystemFn)(void);
        auto getSysDpi = u32 ? (GetDpiForSystemFn)GetProcAddress(u32, "GetDpiForSystem") : nullptr;
        UINT dpi0 = getSysDpi ? getSysDpi() : 96;
        if (!dpi0) {
            HDC hdc = GetDC(nullptr);
            if (hdc) { dpi0 = (UINT)GetDeviceCaps(hdc, LOGPIXELSX); ReleaseDC(nullptr, hdc); }
        }
        if (!dpi0) dpi0 = 96;
        winW = MulDiv(920, (int)dpi0, 96);
        winH = MulDiv(560, (int)dpi0, 96);
    }

    g_hwnd = CreateWindowExW(0, L"DiskMateWnd",
                             L"DiskMate — 磁盘空间分析（原生版）",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, winW, winH,
                             nullptr, nullptr, hInstance, nullptr);
    if (!g_hwnd) return 1;
    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    if (!autoScan.empty()) StartScan(autoScan);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(g_hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
