#include "settings.h"
#include "jsonlite.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <map>
#include <vector>

// JSON 解析/序列化与文件读写已抽到 jsonlite（见 jsonlite.h/.cpp），
// 本文件只负责 AppSettings 的字段绑定。

// ===================== 设置: JSON 双向绑定 =====================

std::wstring GetConfigPath() {
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n && n < MAX_PATH) {
        std::wstring dir = exe;
        size_t pos = dir.find_last_of(L"\\/");
        if (pos != std::wstring::npos) dir = dir.substr(0, pos + 1);
        dir += L"config.json";
        // 测试 exe 目录可写：能创建文件即用
        HANDLE h = CreateFileW(dir.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            return dir;
        }
    }
    wchar_t buf[MAX_PATH];
    if (SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buf) == S_OK) {
        std::wstring p = buf;
        p += L"\\DiskMate";
        CreateDirectoryW(p.c_str(), nullptr);
        p += L"\\config.json";
        return p;
    }
    return L"config.json";
}

static const jsonlite::Value& Child(const jsonlite::Value& v, const wchar_t* key) {
    static const jsonlite::Value kNull;
    auto it = v.obj.find(key);
    return it == v.obj.end() ? kNull : it->second;
}

static int Clamp(int v, int lo, int hi) {
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

void LoadSettings(AppSettings& s) {
    s = AppSettings{};  // 1) 先落内置默认值
    std::wstring path = GetConfigPath();
    std::wstring text = jsonlite::ReadFile(path);
    if (text.empty()) {
        SaveSettings(s);  // 2) 无文件 → 写默认值文件
        return;
    }
    jsonlite::Value root = jsonlite::Parse(text);
    if (root.kind != jsonlite::Value::OBJ) {
        SaveSettings(s);  // 损坏 → 重建默认
        return;
    }
    const jsonlite::Value& scan = Child(root, L"scan");
    s.threads = Clamp(Child(scan, L"threads").asInt(8), 1, 16);
    s.skipHidden = Child(scan, L"skipHidden").asBool(false);
    s.followReparse = Child(scan, L"followReparse").asBool(false);

    const jsonlite::Value& tm = Child(root, L"treemap");
    s.maxDepth = Clamp(Child(tm, L"maxDepth").asInt(10), 0, 999);  // 0=无限层级
    s.minSizeMB = Clamp(Child(tm, L"minSizeMB").asInt(1), 0, 8192);  // 0=全部
    s.maxRects = Clamp(Child(tm, L"maxRects").asInt(2000), 0, 50000);  // 0=不限
    s.kidsCap = Clamp(Child(tm, L"kidsCap").asInt(120), 0, 500);  // 0=不限
    s.lazyDepth = Clamp(Child(tm, L"lazyDepth").asInt(0), 0, 99);  // 0=全量；层级之下懒加载
    s.showLabels = Child(tm, L"showLabels").asBool(true);
    s.colorScheme = Clamp(Child(tm, L"colorScheme").asInt(0), 0, 2);
    s.mapKind = Clamp(Child(tm, L"mapKind").asInt(0), 0, 4);
    s.ordKind = Clamp(Child(tm, L"ordKind").asInt(0), 0, 2);
    { double pa = Child(tm, L"powAlpha").asNum(0.5); s.powAlpha = (pa < 0.1) ? 0.1 : (pa > 1.0 ? 1.0 : pa); }

    const jsonlite::Value& ui = Child(root, L"ui");
    s.defaultSort = Clamp(Child(ui, L"defaultSort").asInt(0), 0, 1);
    s.rowExtraPx = Clamp(Child(ui, L"rowExtraPx").asInt(16), 0, 48);
    s.theme = Clamp(Child(ui, L"theme").asInt(0), 0, 1);
    s.confirmDelete = Child(ui, L"confirmDelete").asBool(true);

    const jsonlite::Value& menu = Child(root, L"contextMenu");
    MenuConfig& m = s.menu;
    m.open = Child(menu, L"open").asBool(true);
    m.explore = Child(menu, L"explore").asBool(true);
    m.terminal = Child(menu, L"terminal").asBool(true);
    m.props = Child(menu, L"props").asBool(true);
    m.copyPath = Child(menu, L"copyPath").asBool(true);
    m.copyName = Child(menu, L"copyName").asBool(true);
    m.copySize = Child(menu, L"copySize").asBool(true);
    m.enter = Child(menu, L"enter").asBool(true);
    m.locateList = Child(menu, L"locateList").asBool(true);
    m.locateTreemap = Child(menu, L"locateTreemap").asBool(true);
    m.selectParent = Child(menu, L"selectParent").asBool(true);
    m.goUp = Child(menu, L"goUp").asBool(true);
    m.back = Child(menu, L"back").asBool(true);
    m.forward = Child(menu, L"forward").asBool(true);
    m.rescan = Child(menu, L"rescan").asBool(true);
    m.deleteRecycle = Child(menu, L"deleteRecycle").asBool(true);
    m.deleteForever = Child(menu, L"deleteForever").asBool(true);
    m.shellMenu = Child(menu, L"shellMenu").asBool(true);

    s.lastPath = Child(root, L"lastPath").asStr(L"");
}

static jsonlite::Value BoolV(bool b) {
    jsonlite::Value v;
    v.kind = jsonlite::Value::BOOL;
    v.b = b;
    return v;
}
static jsonlite::Value IntV(int i) {
    jsonlite::Value v;
    v.kind = jsonlite::Value::NUM;
    v.num = i;
    return v;
}
static jsonlite::Value StrV(const std::wstring& s) {
    jsonlite::Value v;
    v.kind = jsonlite::Value::STR;
    v.str = s;
    return v;
}

void SaveSettings(const AppSettings& s) {
    jsonlite::Value root;
    root.kind = jsonlite::Value::OBJ;

    jsonlite::Value scan;
    scan.kind = jsonlite::Value::OBJ;
    scan.obj[L"threads"] = IntV(s.threads);
    scan.obj[L"skipHidden"] = BoolV(s.skipHidden);
    scan.obj[L"followReparse"] = BoolV(s.followReparse);
    root.obj[L"scan"] = scan;

    jsonlite::Value tm;
    tm.kind = jsonlite::Value::OBJ;
    tm.obj[L"maxDepth"] = IntV(s.maxDepth);
    tm.obj[L"minSizeMB"] = IntV(s.minSizeMB);
    tm.obj[L"maxRects"] = IntV(s.maxRects);
    tm.obj[L"kidsCap"] = IntV(s.kidsCap);
    tm.obj[L"lazyDepth"] = IntV(s.lazyDepth);
    tm.obj[L"showLabels"] = BoolV(s.showLabels);
    tm.obj[L"colorScheme"] = IntV(s.colorScheme);
    tm.obj[L"mapKind"] = IntV(s.mapKind);
    tm.obj[L"ordKind"] = IntV(s.ordKind);
    tm.obj[L"powAlpha"] = jsonlite::MakeNum(s.powAlpha);
    root.obj[L"treemap"] = tm;

    jsonlite::Value ui;
    ui.kind = jsonlite::Value::OBJ;
    ui.obj[L"defaultSort"] = IntV(s.defaultSort);
    ui.obj[L"rowExtraPx"] = IntV(s.rowExtraPx);
    ui.obj[L"theme"] = IntV(s.theme);
    ui.obj[L"confirmDelete"] = BoolV(s.confirmDelete);
    root.obj[L"ui"] = ui;

    const MenuConfig& m = s.menu;
    jsonlite::Value menu;
    menu.kind = jsonlite::Value::OBJ;
    menu.obj[L"open"] = BoolV(m.open);
    menu.obj[L"explore"] = BoolV(m.explore);
    menu.obj[L"terminal"] = BoolV(m.terminal);
    menu.obj[L"props"] = BoolV(m.props);
    menu.obj[L"copyPath"] = BoolV(m.copyPath);
    menu.obj[L"copyName"] = BoolV(m.copyName);
    menu.obj[L"copySize"] = BoolV(m.copySize);
    menu.obj[L"enter"] = BoolV(m.enter);
    menu.obj[L"locateList"] = BoolV(m.locateList);
    menu.obj[L"locateTreemap"] = BoolV(m.locateTreemap);
    menu.obj[L"selectParent"] = BoolV(m.selectParent);
    menu.obj[L"goUp"] = BoolV(m.goUp);
    menu.obj[L"back"] = BoolV(m.back);
    menu.obj[L"forward"] = BoolV(m.forward);
    menu.obj[L"rescan"] = BoolV(m.rescan);
    menu.obj[L"deleteRecycle"] = BoolV(m.deleteRecycle);
    menu.obj[L"deleteForever"] = BoolV(m.deleteForever);
    menu.obj[L"shellMenu"] = BoolV(m.shellMenu);
    root.obj[L"contextMenu"] = menu;

    root.obj[L"lastPath"] = StrV(s.lastPath);

    jsonlite::WriteFile(GetConfigPath(), jsonlite::Dump(root));
}

// ===================== 模态窗口通用 =====================

static void RunModalLoop(HWND dlg) {
    MSG msg;
    while (IsWindow(dlg)) {
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                PostQuitMessage((int)msg.wParam);
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        } else {
            WaitMessage();
        }
    }
}

static void EnsureClass(HINSTANCE h, const wchar_t* name, WNDPROC proc) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = h;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = name;
    RegisterClassExW(&wc);  // 重复注册可忽略
}

// 打开模态子窗口并跑消息循环；返回窗口句柄（销毁后仍可读 DlgData.ok）
static HWND OpenModal(HWND parent, const wchar_t* cls, const wchar_t* title, int w, int h,
                      WNDPROC proc, void* data) {
    HINSTANCE hi = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    EnsureClass(hi, cls, proc);
    EnableWindow(parent, FALSE);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, cls, title, WS_CAPTION | WS_SYSMENU, 0, 0,
                               w, h, parent, nullptr, hi, data);
    if (!dlg) {
        EnableWindow(parent, TRUE);
        return nullptr;
    }
    RECT pr, dr;
    GetWindowRect(parent, &pr);
    GetWindowRect(dlg, &dr);
    int x = pr.left + ((pr.right - pr.left) - (dr.right - dr.left)) / 2;
    int y = pr.top + ((pr.bottom - pr.top) - (dr.bottom - dr.top)) / 2;
    SetWindowPos(dlg, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);
    RunModalLoop(dlg);
    return dlg;
}

// ===================== 设置对话框 =====================

const int E_DEPTH = 201, E_MIN = 202, E_RECTS = 203, E_KIDS = 204, E_THREADS = 205;
const int C_HIDDEN = 206, C_REPARSE = 207, C_LABELS = 208, C_CONFIRM = 209;
const int CB_SCHEME = 210, CB_SORT = 211;
const int E_ROWEXTRA = 212;
const int CB_THEME = 213;
const int B_OK = 220, B_CANCEL = 221, B_MENU = 222;

struct DlgData {
    HWND parent;
    AppSettings draft;
    bool ok = false;
    double sc = 1.0;          // DPI 缩放
    HFONT titleFont = nullptr;  // 分区标题字体
    HBRUSH bg = nullptr;      // 窗口背景（深色）
    HBRUSH editBg = nullptr;  // 编辑框背景
    HBRUSH accent = nullptr;  // 强调色（确定按钮）
};

int ParseInt(HWND e, int fallback) {
    wchar_t buf[64];
    GetWindowTextW(e, buf, 64);
    int v;
    if (swscanf(buf, L"%d", &v) == 1) return v;
    return fallback;
}

LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            auto* cs = (CREATESTRUCTW*)lParam;
            auto* d = (DlgData*)cs->lpCreateParams;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)d);
            HINSTANCE h = cs->hInstance;
            HDC sdc = GetDC(hwnd);
            d->sc = GetDeviceCaps(sdc, LOGPIXELSX) / 96.0;
            ReleaseDC(hwnd, sdc);
            if (d->sc < 1.0) d->sc = 1.0;
            d->bg = CreateSolidBrush(RGB(28, 31, 36));
            d->editBg = CreateSolidBrush(RGB(38, 42, 48));
            d->accent = CreateSolidBrush(RGB(45, 105, 255));
            d->titleFont = CreateFontW(-(int)(13 * d->sc + 0.5), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                                       DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            auto S = [&](int v) { return (int)(v * d->sc + 0.5); };
            auto title = [&](const wchar_t* t, int id, int y) {
                HWND c = CreateWindowExW(0, L"STATIC", t, WS_CHILD | WS_VISIBLE, S(20), S(y),
                                         160, S(20), hwnd, (HMENU)(INT_PTR)id, h, nullptr);
                SendMessageW(c, WM_SETFONT, (WPARAM)d->titleFont, TRUE);
                return c;
            };
            auto label = [&](const wchar_t* t, int x, int y) {
                CreateWindowExW(0, L"STATIC", t, WS_CHILD | WS_VISIBLE, S(x), S(y), 220, S(16),
                                hwnd, nullptr, h, nullptr);
            };
            auto hint = [&](const wchar_t* t, int id, int x, int y) {
                CreateWindowExW(0, L"STATIC", t, WS_CHILD | WS_VISIBLE, S(x), S(y), 160, S(16),
                                hwnd, (HMENU)(INT_PTR)id, h, nullptr);
            };
            auto edit = [&](int id, const std::wstring& v, int x, int y, int w = 62) {
                CreateWindowExW(0, L"EDIT", v.c_str(),
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER, S(x), S(y),
                                S(w), S(22), hwnd, (HMENU)(INT_PTR)id, h, nullptr);
            };
            auto chk = [&](int id, const wchar_t* t, bool on, int x, int y) {
                HWND c = CreateWindowExW(0, L"BUTTON", t,
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                         S(x), S(y), 210, S(20), hwnd, (HMENU)(INT_PTR)id, h,
                                         nullptr);
                SendMessageW(c, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
            };
            auto combo = [&](int id, int x, int y,
                             std::initializer_list<const wchar_t*> items, int sel) {
                HWND c = CreateWindowExW(0, L"COMBOBOX", L"",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                                         S(x), S(y), S(176), 220, hwnd, (HMENU)(INT_PTR)id, h,
                                         nullptr);
                for (auto s : items) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)s);
                SendMessageW(c, CB_SETCURSEL, sel, 0);
                return c;
            };
            auto btn = [&](int id, const wchar_t* t, int x, int y, int w = 84, bool def = false) {
                CreateWindowExW(0, L"BUTTON", t,
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                    (def ? BS_DEFPUSHBUTTON : 0),
                                S(x), S(y), S(w), S(26), hwnd, (HMENU)(INT_PTR)id, h, nullptr);
            };

            // ---- 扫描 ----
            title(L"扫描", 601, 14);
            label(L"扫描线程数 (1-16)", 20, 42);
            edit(E_THREADS, std::to_wstring(d->draft.threads), 20, 60, 62);
            hint(L"并行扫描速度", 501, 90, 64);
            chk(C_HIDDEN, L"跳过隐藏/系统项", d->draft.skipHidden, 210, 42);
            chk(C_REPARSE, L"跟随重解析点（自动防环）", d->draft.followReparse, 210, 64);

            // ---- 树图 ----
            title(L"树图", 602, 92);
            label(L"递归层级", 20, 120);
            edit(E_DEPTH, std::to_wstring(d->draft.maxDepth), 20, 138, 62);
            hint(L"0 = 无限层级", 502, 90, 142);
            label(L"递归最小尺寸", 190, 120);
            edit(E_MIN, std::to_wstring(d->draft.minSizeMB), 190, 138, 62);
            hint(L"MB · 0 = 全部", 503, 260, 142);
            label(L"最大矩形数", 20, 168);
            edit(E_RECTS, std::to_wstring(d->draft.maxRects), 20, 186, 62);
            label(L"每文件夹展开上限", 190, 168);
            edit(E_KIDS, std::to_wstring(d->draft.kidsCap), 190, 186, 62);
            chk(C_LABELS, L"大矩形显示名称文字", d->draft.showLabels, 20, 214);
            label(L"配色方案", 190, 214);
            combo(CB_SCHEME, 190, 232, { L"按扩展名", L"按类型分组", L"单色" },
                  d->draft.colorScheme);

            // ---- 操作 ----
            title(L"操作", 603, 264);
            label(L"列表默认排序", 20, 292);
            combo(CB_SORT, 20, 310, { L"大小降序", L"名称升序" }, d->draft.defaultSort);
            label(L"列表附加行高", 20, 348);
            edit(E_ROWEXTRA, std::to_wstring(d->draft.rowExtraPx), 20, 366, 62);
            hint(L"px · 0 = 单行紧凑", 505, 90, 370);
            label(L"界面主题", 190, 348);
            combo(CB_THEME, 190, 366, { L"深色", L"浅色" }, d->draft.theme);
            chk(C_CONFIRM, L"删除到回收站前由系统确认", d->draft.confirmDelete, 210, 292);
            btn(B_MENU, L"右键菜单…", 210, 310, 120);
            hint(L"开关与条目均保存于 config.json", 504, 210, 388);

            // ---- 底部按钮 ----
            btn(B_OK, L"确定", 250, 424, 84, true);
            btn(B_CANCEL, L"取消", 342, 424, 84);
            return 0;
        }
        case WM_ERASEBKGND: {  // 深色背景
            auto* d = (DlgData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect((HDC)wParam, &rc, d ? d->bg : (HBRUSH)GetStockObject(BLACK_BRUSH));
            return 1;
        }
        case WM_PAINT: {  // 分区分隔线
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            auto* d = (DlgData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            if (d) {
                HPEN pen = CreatePen(PS_SOLID, 1, RGB(58, 64, 72));
                HGDIOBJ op = SelectObject(dc, pen);
                for (int y : { 40, 92, 264, 340 }) {
                    int yy = (int)((y + 22) * d->sc + 0.5);
                    MoveToEx(dc, 20, yy, nullptr);
                    LineTo(dc, 420, yy);
                }
                SelectObject(dc, op);
                DeleteObject(pen);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wParam;
            int id = GetDlgCtrlID((HWND)lParam);
            if (id >= 601) SetTextColor(dc, RGB(235, 240, 245));       // 分区标题
            else if (id >= 501) SetTextColor(dc, RGB(140, 150, 162));  // 说明文字
            else SetTextColor(dc, RGB(200, 208, 218));                 // 普通标签
            SetBkMode(dc, TRANSPARENT);
            return (LRESULT)GetStockObject(NULL_BRUSH);
        }
        case WM_CTLCOLOREDIT: {
            auto* d = (DlgData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            HDC dc = (HDC)wParam;
            SetTextColor(dc, RGB(235, 240, 245));
            SetBkColor(dc, RGB(38, 42, 48));
            return (LRESULT)(d ? d->editBg : GetStockObject(BLACK_BRUSH));
        }
        case WM_CTLCOLORBTN: {
            auto* d = (DlgData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            HDC dc = (HDC)wParam;
            HWND c = (HWND)lParam;
            if (GetDlgCtrlID(c) == B_OK) {  // 确定=强调色
                SetTextColor(dc, RGB(255, 255, 255));
                SetBkColor(dc, RGB(45, 105, 255));
                return (LRESULT)(d ? d->accent : GetStockObject(BLACK_BRUSH));
            }
            SetTextColor(dc, RGB(210, 218, 226));
            SetBkColor(dc, RGB(38, 42, 48));
            return (LRESULT)(d ? d->editBg : GetStockObject(BLACK_BRUSH));
        }
        case WM_CTLCOLORDLG: {
            auto* d = (DlgData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            return (LRESULT)(d ? d->bg : GetStockObject(BLACK_BRUSH));
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            auto* d = (DlgData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            if (!d) break;
            if (id == B_OK) {
                auto editVal = [&](int eid, int fallback, int minV, int maxV) {
                    int v = ParseInt(GetDlgItem(hwnd, eid), fallback);
                    return Clamp(v, minV, maxV);
                };
                auto chkVal = [&](int cid) {
                    return SendMessageW(GetDlgItem(hwnd, cid), BM_GETCHECK, 0, 0) ==
                           BST_CHECKED;
                };
                auto comboVal = [&](int cid) {
                    LRESULT s = SendMessageW(GetDlgItem(hwnd, cid), CB_GETCURSEL, 0, 0);
                    return (int)(s == CB_ERR ? 0 : s);
                };
                d->draft.threads = editVal(E_THREADS, 8, 1, 16);
                d->draft.skipHidden = chkVal(C_HIDDEN);
                d->draft.followReparse = chkVal(C_REPARSE);
                d->draft.maxDepth = editVal(E_DEPTH, 10, 0, 999);  // 0=无限层级
                d->draft.minSizeMB = editVal(E_MIN, 1, 0, 8192);
                d->draft.maxRects = editVal(E_RECTS, 2000, 100, 50000);
                d->draft.kidsCap = editVal(E_KIDS, 120, 20, 500);
                d->draft.showLabels = chkVal(C_LABELS);
                d->draft.colorScheme = comboVal(CB_SCHEME);
                d->draft.defaultSort = comboVal(CB_SORT);
                d->draft.rowExtraPx = editVal(E_ROWEXTRA, 16, 0, 48);
                d->draft.theme = comboVal(CB_THEME);
                d->draft.confirmDelete = chkVal(C_CONFIRM);
                d->ok = true;
                DestroyWindow(hwnd);
            } else if (id == B_CANCEL) {
                DestroyWindow(hwnd);
            } else if (id == B_MENU) {
                // 子对话框：修改 draft.menu，确定后写回
                if (ShowMenuConfigDialog(hwnd, d->draft.menu)) {
                    // 菜单配置已在 ShowMenuConfigDialog 内写回 draft
                }
            }
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY: {
            auto* d = (DlgData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            if (d) {
                EnableWindow(d->parent, TRUE);
                SetForegroundWindow(d->parent);
                if (d->titleFont) DeleteObject(d->titleFont);
                if (d->bg) DeleteObject(d->bg);
                if (d->editBg) DeleteObject(d->editBg);
                if (d->accent) DeleteObject(d->accent);
            }
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool ShowSettingsDialog(HWND parent, AppSettings& s) {
    DlgData d{ parent, s, false };
    HDC sdc = GetDC(parent);
    double sc = GetDeviceCaps(sdc, LOGPIXELSX) / 96.0;
    ReleaseDC(parent, sdc);
    if (sc < 1.0) sc = 1.0;
    HWND dlg = OpenModal(parent, L"DiskMateSettings", L"设置", (int)(440 * sc + 0.5),
                         (int)(494 * sc + 0.5), SettingsProc, &d);
    if (!dlg) return false;
    if (d.ok) {
        s = d.draft;
        SaveSettings(s);  // 双向绑定：确定后写回 JSON
    }
    return d.ok;
}

// ===================== 右键菜单配置对话框 =====================

struct MenuSpec {
    const wchar_t* label;
    bool MenuConfig::* field;
};

static const MenuSpec kMenuSpecs[] = {
    { L"打开", &MenuConfig::open },
    { L"在资源管理器中打开", &MenuConfig::explore },
    { L"打开终端", &MenuConfig::terminal },
    { L"属性", &MenuConfig::props },
    { L"复制完整路径", &MenuConfig::copyPath },
    { L"复制文件名", &MenuConfig::copyName },
    { L"复制大小", &MenuConfig::copySize },
    { L"进入此目录", &MenuConfig::enter },
    { L"在列表中定位", &MenuConfig::locateList },
    { L"在树图中定位", &MenuConfig::locateTreemap },
    { L"选择父级", &MenuConfig::selectParent },
    { L"向上", &MenuConfig::goUp },
    { L"后退", &MenuConfig::back },
    { L"前进", &MenuConfig::forward },
    { L"重新扫描此项", &MenuConfig::rescan },
    { L"删除到回收站", &MenuConfig::deleteRecycle },
    { L"永久删除", &MenuConfig::deleteForever },
    { L"资源管理器扩展菜单", &MenuConfig::shellMenu },
};

const int MC_BASE = 300;   // +0..17
const int MC_OK = 330, MC_CANCEL = 331;

struct MenuDlgData {
    HWND parent;
    MenuConfig* m;
    bool ok = false;
};

LRESULT CALLBACK MenuConfigProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            auto* cs = (CREATESTRUCTW*)lParam;
            auto* d = (MenuDlgData*)cs->lpCreateParams;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)d);
            HINSTANCE h = cs->hInstance;
            CreateWindowExW(0, L"STATIC",
                            L"勾选显示的命令；取消后该项不再出现在右键菜单。",
                            WS_CHILD | WS_VISIBLE, 18, 14, 430, 16, hwnd, nullptr, h, nullptr);
            const int n = (int)(sizeof(kMenuSpecs) / sizeof(kMenuSpecs[0]));
            for (int i = 0; i < n; i++) {
                int col = i / 9, row = i % 9;
                bool on = d->m->*(kMenuSpecs[i].field);
                HWND c = CreateWindowExW(0, L"BUTTON", kMenuSpecs[i].label,
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                         18 + col * 215, 36 + row * 24, 205, 20, hwnd,
                                         (HMENU)(INT_PTR)(MC_BASE + i), h, nullptr);
                SendMessageW(c, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
            }
            CreateWindowExW(0, L"BUTTON", L"确定",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 170, 260,
                            84, 26, hwnd, (HMENU)(INT_PTR)MC_OK, h, nullptr);
            CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 266, 260,
                            84, 26, hwnd, (HMENU)(INT_PTR)MC_CANCEL, h, nullptr);
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            auto* d = (MenuDlgData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            if (!d) break;
            if (id == MC_OK) {
                const int n = (int)(sizeof(kMenuSpecs) / sizeof(kMenuSpecs[0]));
                for (int i = 0; i < n; i++) {
                    bool on = SendMessageW(GetDlgItem(hwnd, MC_BASE + i), BM_GETCHECK, 0, 0) ==
                               BST_CHECKED;
                    d->m->*(kMenuSpecs[i].field) = on;
                }
                d->ok = true;
                DestroyWindow(hwnd);
            } else if (id == MC_CANCEL) {
                DestroyWindow(hwnd);
            }
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY: {
            auto* d = (MenuDlgData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            if (d) {
                EnableWindow(d->parent, TRUE);
                SetForegroundWindow(d->parent);
            }
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool ShowMenuConfigDialog(HWND parent, MenuConfig& m) {
    MenuDlgData d{ parent, &m, false };
    HWND dlg = OpenModal(parent, L"DiskMateMenuCfg", L"右键菜单", 460, 320, MenuConfigProc,
                         &d);
    if (!dlg) return false;
    return d.ok;
}

// ===================== 重命名对话框 =====================

const int R_EDIT = 230, R_OK = 231, R_CANCEL = 232;

struct RenameData {
    HWND parent;
    std::wstring* name;
    bool ok = false;
};

LRESULT CALLBACK RenameProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            auto* cs = (CREATESTRUCTW*)lParam;
            auto* d = (RenameData*)cs->lpCreateParams;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)d);
            HINSTANCE h = cs->hInstance;
            CreateWindowExW(0, L"STATIC", L"新名称:", WS_CHILD | WS_VISIBLE, 16, 20, 90, 18,
                            hwnd, nullptr, h, nullptr);
            CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", d->name->c_str(),
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 16, 42, 240,
                            22, hwnd, (HMENU)(INT_PTR)R_EDIT, h, nullptr);
            CreateWindowExW(0, L"BUTTON", L"确定",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 88, 84, 84,
                            26, hwnd, (HMENU)(INT_PTR)R_OK, h, nullptr);
            CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 184, 84,
                            84, 26, hwnd, (HMENU)(INT_PTR)R_CANCEL, h, nullptr);
            SetFocus(GetDlgItem(hwnd, R_EDIT));
            SendMessageW(GetDlgItem(hwnd, R_EDIT), EM_SETSEL, 0, -1);
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            auto* d = (RenameData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            if (!d) break;
            if (id == R_OK) {
                wchar_t buf[512];
                GetWindowTextW(GetDlgItem(hwnd, R_EDIT), buf, 512);
                std::wstring v = buf;
                while (!v.empty() && v.front() == L' ') v.erase(v.begin());
                while (!v.empty() && v.back() == L' ') v.pop_back();
                if (!v.empty() && v.find_first_of(L"\\/") == std::wstring::npos) {
                    *d->name = v;
                    d->ok = true;
                    DestroyWindow(hwnd);
                }
            } else if (id == R_CANCEL) {
                DestroyWindow(hwnd);
            }
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY: {
            auto* d = (RenameData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            if (d) {
                EnableWindow(d->parent, TRUE);
                SetForegroundWindow(d->parent);
            }
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool ShowRenameDialog(HWND parent, std::wstring& name) {
    RenameData d{ parent, &name, false };
    HWND dlg = OpenModal(parent, L"DiskMateRename", L"重命名", 300, 140, RenameProc, &d);
    if (!dlg) return false;
    return d.ok;
}
