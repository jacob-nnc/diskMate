// webview_host.cpp — WebView2 宿主：UI 交给 HTML/CSS/JS，C++ 只做扫描与桥接
//
// 设计：
//   - 窗口里只有一个 WebView2 控件，界面全部由 web/index.html 渲染
//   - C++ → JS：ExecuteScript 调用页面的 window.__host.* 入口
//   - JS → C++：页面 postMessage，宿主在 WebMessageReceived 里分发命令
//   - 扫描逻辑复用现有 scanner.cpp（多线程），不改动
//
// 说明：不用 WRL 的 Callback（MinGW 不支持），改手写 COM 回调类。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <share.h>
#include <shellapi.h>
#ifndef SEE_MASK_ASYNC
#define SEE_MASK_ASYNC 0x00000001
#endif
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <unordered_set>
#include <atomic>
#include <algorithm>
#include <string>
#include <vector>

#include "WebView2.h"
#include "scanner.h"
#include "scanner_M.h"   // MFT reader (NTFS volume, admin required; switchable with normal walk)
#include "utils.h"
#include "settings.h"

#define IDI_DISKMATE 101        // app.rc: IDI_ICON ICON "diskmate_tree.ico"

#pragma comment(lib, "shlwapi.lib")

// MinGW 没有 __uuidof 的自动实现：为用到的接口显式声明 IID
#ifdef __MINGW32__
#include <initguid.h>
__CRT_UUID_DECL(ICoreWebView2Environment, 0xb96d755e, 0x0319, 0x4e92, 0xa2, 0x96, 0x23, 0x43, 0x0f, 0x1e, 0x8f, 0xcb)
__CRT_UUID_DECL(ICoreWebView2Controller, 0x4d00c0d1, 0x9434, 0x4eb6, 0x80, 0x78, 0x86, 0x97, 0xa5, 0x70, 0x32, 0xe6)
__CRT_UUID_DECL(ICoreWebView2, 0x76eceacb, 0x0462, 0x4d94, 0xac, 0x83, 0x42, 0x3a, 0x67, 0x93, 0x77, 0x5e)
__CRT_UUID_DECL(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, 0x4e8a3389, 0xc9d8, 0x4bd2, 0xb6, 0xb5, 0x12, 0x4f, 0xee, 0x6c, 0xc1, 0x4d)
__CRT_UUID_DECL(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, 0x6c4819f3, 0xc9b7, 0x4260, 0x81, 0x27, 0xc9, 0xf5, 0xbd, 0xe7, 0xf6, 0xde)
__CRT_UUID_DECL(ICoreWebView2WebMessageReceivedEventHandler, 0x57213f19, 0x00e6, 0x49fa, 0x8e, 0x07, 0x89, 0x8e, 0xa0, 0x1e, 0xcb, 0xd2)
__CRT_UUID_DECL(ICoreWebView2WebMessageReceivedEventArgs, 0x0f99a40c, 0xe962, 0x4207, 0x9e, 0x3d, 0xe9, 0x20, 0x61, 0x9c, 0x5a, 0x93)
#endif

// ============================================================================
// 手写 COM 回调（MinGW 无 WRL::Callback）：每个接口一个独立的类
// ============================================================================
#define DECL_HANDLER(NAME, IFACE, FN, SIG, CALL)                                  \
    class NAME : public IFACE {                                                   \
    public:                                                                       \
        explicit NAME(FN fn) : fn_(fn), ref_(1) {}                                 \
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override { \
            if (!ppv) return E_POINTER;                                            \
            if (riid == IID_IUnknown || riid == __uuidof(IFACE)) {                 \
                *ppv = static_cast<IFACE*>(this);                                 \
                AddRef();                                                         \
                return S_OK;                                                      \
            }                                                                     \
            *ppv = nullptr;                                                       \
            return E_NOINTERFACE;                                                 \
        }                                                                         \
        ULONG STDMETHODCALLTYPE AddRef() override {                                \
            return InterlockedIncrement(&ref_);                                    \
        }                                                                         \
        ULONG STDMETHODCALLTYPE Release() override {                               \
            LONG n = InterlockedDecrement(&ref_);                                  \
            if (n == 0) delete this;                                              \
            return n;                                                             \
        }                                                                         \
        HRESULT STDMETHODCALLTYPE Invoke SIG override { return fn_ CALL; }         \
                                                                                   \
    private:                                                                      \
        FN fn_;                                                                   \
        LONG ref_;                                                                \
    };

using EnvFn = std::function<HRESULT(HRESULT, ICoreWebView2Environment*)>;
using CtlFn = std::function<HRESULT(HRESULT, ICoreWebView2Controller*)>;
using MsgFn = std::function<HRESULT(ICoreWebView2*,
                                    ICoreWebView2WebMessageReceivedEventArgs*)>;

DECL_HANDLER(EnvDone, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, EnvFn,
             (HRESULT hr, ICoreWebView2Environment* e), (hr, e))
DECL_HANDLER(CtlDone, ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, CtlFn,
             (HRESULT hr, ICoreWebView2Controller* c), (hr, c))
DECL_HANDLER(MsgRecv, ICoreWebView2WebMessageReceivedEventHandler, MsgFn,
             (ICoreWebView2* s, ICoreWebView2WebMessageReceivedEventArgs* a), (s, a))

// ---------- 全局 ----------
#define WM_APP_WEBJSON (WM_APP + 20)   // wParam=0, lParam=std::wstring*（UI 线程转发给页面）
#define WM_APP_PROGRESS (WM_APP + 21)  // 进度合并：wParam/lParam 存 items/bytes，UI 线程节流后发
#define WM_APP_SHELLMENU (WM_APP + 22) // 壳菜单：lParam=std::wstring* 路径（在消息循环里执行）
static HWND g_hwnd = nullptr;
// 进度合并槽（只保留最新值；UI 线程按节流频率发一次）
static volatile LONG g_progPending = 0;
static volatile ULONGLONG g_progItems = 0, g_progBytes = 0;
static UINT_PTR g_progTimer = 0;
static ICoreWebView2Controller* g_controller = nullptr;
static ICoreWebView2* g_webview = nullptr;
static double g_dpiScale = 1.0;   // 供页面 canvas 用（devicePixelRatio 在 WebView2 里可能不准）
// 完整树落盘（full_mft.json / full_walk.json）默认关闭，加 --dumpfulltree 才写。
// 理由：前端一次都不读它（web/index.html 零引用），只用于 MFT/walk 一致性核对；
// 而默认开启意味着每次扫描都在 exe 旁边写 70~100MB，写法还是
// “先写 <file>.tmp → MoveFileEx 覆盖目标 → 删 tmp”，属于杀软启发式里权重很高的形态。
static bool g_dumpFullTree = false;

// ---------- 工具 ----------
static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p = buf;
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"" : p.substr(0, s + 1);
}

// ---------- 日志（诊断用） ----------
// 诊断日志：设环境变量 DISKMATE_LOG=1 打开（默认关闭，不写文件）
static bool LogEnabled() {
    // 日志默认开启：诊断能力不应依赖环境变量（所有用户都可能遇到问题）
    return true;
}
static void LogLine(const std::wstring& s) {
    if (!LogEnabled()) return;
    static FILE* fp = nullptr;
    if (!fp) {
        std::wstring p = ExeDir() + L"diskmate.log";
        // 轮转：超过 4MB 删掉重建，防止日志无限膨胀
        WIN32_FILE_ATTRIBUTE_DATA lfd{};
        if (GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &lfd) &&
            (((ULONGLONG)lfd.nFileSizeHigh << 32) | lfd.nFileSizeLow) > 4ULL * 1024 * 1024)
            DeleteFileW(p.c_str());
        fp = _wfsopen(p.c_str(), L"a, ccs=UTF-8", _SH_DENYNO);
    }
    if (!fp) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fwprintf(fp, L"[%02d:%02d:%02d] %ls\n", st.wHour, st.wMinute, st.wSecond, s.c_str());
    fflush(fp);
}

// 转义成 JS 字符串字面量内容
static std::wstring JsEscape(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size() + 8);
    wchar_t buf[8];
    for (wchar_t c : in) {
        switch (c) {
            case L'\\': out += L"\\\\"; break;
            case L'"': out += L"\\\""; break;
            case L'\n': out += L"\\n"; break;
            case L'\r': out += L"\\r"; break;
            case L'\t': out += L"\\t"; break;
            case L'\b': out += L"\\b"; break;
            case L'\f': out += L"\\f"; break;
            default:
                // 其余控制字符（NTFS 文件名可能含 \\x00-\\x1F）转 \\uXXXX，否则 PostWebMessageAsJson 返回 E_INVALIDARG 拒收整条消息（踩过）
                if (c < 0x20) {
                    swprintf(buf, 8, L"\\u%04x", (unsigned)c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    return out;
}
static void EvalJs(const std::wstring& js) {
    if (!g_webview) { LogLine(L"EvalJs SKIP (no webview): " + js.substr(0,60)); return; }
    HRESULT hr = g_webview->ExecuteScript(js.c_str(), nullptr);
    if (FAILED(hr)) LogLine(L"EvalJs FAIL hr=" + std::to_wstring(hr) + L" : " + js.substr(0,60));
}

static void OnWebViewReady();
static void LoadLibFile();
static void OnMessageFromPage(const std::wstring& json);

static bool ShowShellContextMenu(HWND hwnd, const std::wstring& path) {
    LogLine(L"shellmenu[1] enter path=" + path);
    PIDLIST_ABSOLUTE pidl = nullptr;
    SFGAOF attr = 0;
    HRESULT hr = SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, &attr);
    LogLine(L"shellmenu[2] SHParseDisplayName hr=0x" +
            std::to_wstring((unsigned long)hr) + L" pidl=" + (pidl ? L"ok" : L"null"));
    if (FAILED(hr) || !pidl) return false;
    bool ok = false;
    IShellFolder* parent = nullptr;
    PCUITEMID_CHILD child = nullptr;
    hr = SHBindToParent(pidl, IID_IShellFolder, (void**)&parent, &child);
    LogLine(L"shellmenu[3] SHBindToParent hr=0x" + std::to_wstring((unsigned long)hr));
    if (SUCCEEDED(hr) && parent) {
        IContextMenu* cm = nullptr;
        hr = parent->GetUIObjectOf(hwnd, 1, &child, IID_IContextMenu, nullptr,
                                   (void**)&cm);
        LogLine(L"shellmenu[4] GetUIObjectOf hr=0x" + std::to_wstring((unsigned long)hr)
                + L" cm=" + (cm ? L"ok" : L"null"));
        if (SUCCEEDED(hr) && cm) {
            HMENU menu = CreatePopupMenu();
            if (menu) {
                // idCmdFirst/idCmdLast 必须给壳扩展一段【它独占】的 id 区间。
                // 用 idCmdFirst=1 时扩展可能写出越界的 offset，InvokeCommand 里
                // 扩展按 offset 查表就越界 → 闪退（踩过）。
                // 取高位做基址，区间收紧到 0x1000 个 id。
                const UINT ID_FIRST = 0x8000, ID_LAST = 0x8FFF;
                LogLine(L"shellmenu[5] before QueryContextMenu path=" + path +
                        L" tid=" + std::to_wstring(GetCurrentThreadId()));
                hr = cm->QueryContextMenu(menu, 0, ID_FIRST, ID_LAST,
                                          CMF_NORMAL);
                LogLine(L"shellmenu[6] QueryContextMenu hr=0x" +
                        std::to_wstring((unsigned long)hr));
                if (SUCCEEDED(hr)) {
                    POINT pt; GetCursorPos(&pt);
                    LogLine(L"shellmenu[7] before TrackPopupMenu items=" +
                            std::to_wstring(GetMenuItemCount(menu)));
                    // owner 传 nullptr：本线程是独立工作线程，主窗口属于 UI 线程，
                    // 跨线程把别人的窗口当 owner 会让菜单显示不出来（踩过：sel=0 且无菜单）。
                    // 传 nullptr 时消息会进本线程队列，所以下面要自己泵消息。
                    // owner 用主窗口 hwnd（实测能弹出菜单；传 nullptr 反而弹不出来，
                    // 因为无 owner 的菜单在本线程拿不到输入焦点）。
                    // 关键：本线程必须先有消息队列（PeekMessage 会创建），
                    // 否则 TrackPopupMenu 会立刻收到"取消"返回 0。
                    MSG msg;
                    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
                    // 菜单 owner 窗口要在前台，否则点空白处菜单不消失
                    SetForegroundWindow(hwnd);
                    UINT sel = TrackPopupMenu(menu,
                        TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN,
                        pt.x, pt.y, 0, hwnd, nullptr);
                    LogLine(L"shellmenu[8] TrackPopupMenu sel=" + std::to_wstring(sel));
                    if (sel) {
                        // 偏移形式：lpVerb = MAKEINTRESOURCE(sel - idCmdFirst)
                        CMINVOKECOMMANDINFOEX info = {};
                        info.cbSize = sizeof(info);
                        info.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
                        info.hwnd = hwnd;
                        info.lpVerbW = MAKEINTRESOURCEW(sel - ID_FIRST);
                        info.lpVerb  = MAKEINTRESOURCEA(sel - ID_FIRST);
                        info.nShow = SW_SHOWNORMAL;
                        info.ptInvoke = pt;
                        LogLine(L"shellmenu[9] before InvokeCommand");
                        HRESULT hr2 = cm->InvokeCommand((LPCMINVOKECOMMANDINFO)&info);
                        LogLine(L"shellmenu[10] InvokeCommand hr=0x" +
                                std::to_wstring((unsigned long)hr2));
                        ok = true;
                    }
                }
                DestroyMenu(menu);
            }
            cm->Release();
        }
        parent->Release();
    }
    CoTaskMemFree(pidl);
    LogLine(L"shellmenu: path=" + path + L" invoked=" + (ok ? L"1" : L"0"));
    return ok;
}

// ---------- WebView2 初始化 ----------
static void InitWebView() {
    std::wstring userData = ExeDir() + L"webview_data";
    auto envDone = new EnvDone([](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
        if (FAILED(hr) || !env) {
            MessageBoxW(g_hwnd, L"WebView2 环境创建失败（是否缺少 WebView2 运行时？）",
                        L"DiskMate", MB_ICONERROR);
            return S_OK;
        }
        env->CreateCoreWebView2Controller(
            g_hwnd, new CtlDone([](HRESULT r2, ICoreWebView2Controller* c) -> HRESULT {
                if (FAILED(r2) || !c) return S_OK;
                g_controller = c;
                g_controller->AddRef();
                if (g_webview) { g_webview->Release(); g_webview = nullptr; }
                HRESULT hg = c->get_CoreWebView2(&g_webview);
                if (g_webview) g_webview->AddRef();   // get_ 返回的引用要自己持有
                LogLine(L"get_CoreWebView2 hr=" + std::to_wstring(hg) +
                        L" ptr=" + std::to_wstring((size_t)(void*)g_webview));
                OnWebViewReady();
                return S_OK;
            }));
        return S_OK;
    });

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userData.c_str(), nullptr, envDone);
    if (FAILED(hr)) {
        MessageBoxW(g_hwnd, L"WebView2 初始化失败", L"DiskMate", MB_ICONERROR);
        envDone->Release();
    }
}

// ---------- 页面就绪 ----------
static void OnWebViewReady() {
    LoadLibFile();        // 软件打开的瞬间读入预字库
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    g_controller->put_Bounds(rc);
    // DPI 适配：不设这个，系统缩放 125%/150% 时 WebView2 会按 100% 渲染再放大 → 整体发糊
    {
        HWND hwndTop = GetAncestor(g_hwnd, GA_ROOT);
        UINT dpi = hwndTop ? GetDpiForWindow(hwndTop) : 96;
        if (dpi == 0) dpi = 96;
        double scale = (double)dpi / 96.0;
        // RasterizationScale 在 ICoreWebView2Controller3 上（较新接口），需要 QueryInterface
        ICoreWebView2Controller3* c3 = nullptr;
        if (SUCCEEDED(g_controller->QueryInterface(
                IID_ICoreWebView2Controller3, (void**)&c3)) && c3) {
            c3->put_RasterizationScale(scale);
            c3->put_ShouldDetectMonitorScaleChanges(TRUE);
            c3->Release();
        } else {
            LogLine(L"Controller3 not available, DPI scale not set");
        }
        g_dpiScale = scale;
        LogLine(L"DPI=" + std::to_wstring(dpi) + L" scale=" + std::to_wstring(scale));
    }
    g_controller->put_IsVisible(TRUE);

    // 关掉 WebView2 自带的 Ctrl+滚轮缩放：默认开启时它会【吞掉】Ctrl+滚轮，
    // 事件传不到页面，导致我们自己的 Ctrl+滚轮框选失效。
    if (g_webview) {
        ICoreWebView2Settings* st = nullptr;
        if (SUCCEEDED(g_webview->get_Settings(&st)) && st) {
            st->put_IsZoomControlEnabled(FALSE);
            // 顺便：禁用右键默认菜单（我们用自己的原生菜单）
            st->put_AreDefaultContextMenusEnabled(FALSE);
            st->Release();
        }
    }

    // JS → C++
    EventRegistrationToken tok{};
    // 把真实 DPI 缩放推给页面（canvas 用它决定画布分辨率，否则发糊）
    {
        wchar_t js[128];
        swprintf(js, 128, L"window.__hostScale=%f;",
                 g_dpiScale > 0 ? g_dpiScale : 1.0);
        g_webview->AddScriptToExecuteOnDocumentCreated(js, nullptr);
    }

    g_webview->add_WebMessageReceived(
        new MsgRecv([](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args)
                        -> HRESULT {
            LPWSTR raw = nullptr;
            if (SUCCEEDED(args->get_WebMessageAsJson(&raw)) && raw) {
                OnMessageFromPage(raw);
                CoTaskMemFree(raw);
            }
            return S_OK;
        }),
        &tok);

    std::wstring page = ExeDir() + L"web\\index.html";
    if (GetFileAttributesW(page.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wstring html = L"<html><body style='font:14px sans-serif;padding:24px;"
                            L"background:#1e2228;color:#e2e8f0'>"
                            L"<h3>找不到页面文件</h3><code>" + JsEscape(page) + L"</code>"
                            L"</body></html>";
        g_webview->NavigateToString(html.c_str());
        return;
    }
    // 无人值守验证：DISKMATE_AUTOSCAN=<路径> —— 必须在导航前注入，否则 load 已跑完
    wchar_t autoPath[512]{};
    DWORD an = 0;
    // 完整树落盘按需开启（见 g_dumpFullTree 的说明）
    g_dumpFullTree = wcsstr(GetCommandLineW(), L"--dumpfulltree") != nullptr;
    {
        LPWSTR cl = GetCommandLineW();
        const wchar_t* mark = wcsstr(cl, L"--autoscan=");
        if (mark) {
            size_t n = wcslen(mark);
            if (n > 11) {
                wcsncpy(autoPath, mark + 11, 511);
                autoPath[511] = 0;
                an = (DWORD)wcslen(autoPath);
            }
        }
    }
    if (an == 0)
        an = GetEnvironmentVariableW(L"DISKMATE_AUTOSCAN", autoPath, 512);
    if (an > 0 && an < 512) {
        LogLine(L"AUTOSCAN path=" + std::wstring(autoPath));
        g_webview->AddScriptToExecuteOnDocumentCreated(
            (L"window.__AUTOSCAN=\"" + JsEscape(autoPath) + L"\";").c_str(), nullptr);
    }
    if (GetEnvironmentVariableW(L"DISKMATE_SELFTEST", nullptr, 0))
        g_webview->AddScriptToExecuteOnDocumentCreated(L"window.__SELFTEST=1;", nullptr);
    if (GetEnvironmentVariableW(L"DISKMATE_GEOM", nullptr, 0))
        g_webview->AddScriptToExecuteOnDocumentCreated(L"window.__GEOM=1;", nullptr);
    if (GetEnvironmentVariableW(L"DISKMATE_DRIFT", nullptr, 0))
        g_webview->AddScriptToExecuteOnDocumentCreated(L"window.__DRIFT=1;", nullptr);
    if (GetEnvironmentVariableW(L"DISKMATE_COLS", nullptr, 0))
        g_webview->AddScriptToExecuteOnDocumentCreated(L"window.__COLS=1;", nullptr);
    if (GetEnvironmentVariableW(L"DISKMATE_PNG", nullptr, 0))
        g_webview->AddScriptToExecuteOnDocumentCreated(L"window.__PNG=1;", nullptr);
    if (GetEnvironmentVariableW(L"DISKMATE_PINDBG", nullptr, 0))
        g_webview->AddScriptToExecuteOnDocumentCreated(L"window.__PINDBG=1;", nullptr);
    if (GetEnvironmentVariableW(L"DISKMATE_LAYERS", nullptr, 0))
        g_webview->AddScriptToExecuteOnDocumentCreated(L"window.__LAYERS=1;", nullptr);
    if (GetEnvironmentVariableW(L"DISKMATE_WHEELDBG", nullptr, 0))
        g_webview->AddScriptToExecuteOnDocumentCreated(
            L"window.__WHEELDBG=function(e){window.chrome.webview.postMessage("
            L"{cmd:'diag',what:'wheel',ctrl:e.ctrlKey,dy:e.deltaY,"
            L"tag:(e.target&&e.target.className)||''});};", nullptr);

    // 本地文件必须是合法 file:// URL（正斜杠）——带 ?v= 参数后 WebView2 按 URL 解析，
    // 反斜杠路径会直接失效（表现为「找不到 html」）
    std::wstring url = page;
    for (auto& ch : url) if (ch == L'\\') ch = L'/';
    if (url.rfind(L"file:", 0) != 0) {
        if (url.size() >= 2 && url[1] == L':') url = L"file:///" + url;   // D:/x → file:///D:/x
        else url = L"file://" + url;
    }
    FILETIME ft{};
    HANDLE hf = CreateFileW(page.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf != INVALID_HANDLE_VALUE) {
        GetFileTime(hf, nullptr, nullptr, &ft);
        CloseHandle(hf);
        ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
        // 时间戳 + 进程启动序号：双重保证不复用缓存
        static int bootSeq = 0;
        url += L"?v=" + std::to_wstring(u.QuadPart) + L"_" +
               std::to_wstring(++bootSeq) + L"_" + std::to_wstring(GetTickCount64());
    }
    LogLine(L"Navigate -> " + url);
    g_webview->Navigate(url.c_str());
}

// ============================================================================
// 扫描：复用 scanner.cpp 的多线程实现，结果转成 JSON 推给页面
// ============================================================================
// 保留最近一次扫描结果（供页面按需查询子目录：JSON 只传有限层，深的靠这里补）
// 应用设置（沿用旧版 settings.cpp 的 JSON 双向绑定；Web 界面用 HTML 面板编辑）。
// 全局一份，启动时 LoadSettings 填默认值 + 读 config.json。
static AppSettings g_settings;
static bool g_settingsLoaded = false;
static AppSettings& S() {
    if (!g_settingsLoaded) { LoadSettings(g_settings); g_settingsLoaded = true; }
    return g_settings;
}

static ScanResult g_tree;                       // 拥有全部节点
static std::unordered_map<long long, ScanNode*> g_idMap;
// 预字库：diskmate.lib（UTF-8 每行一词，字典序）。启动时读入；
// 扫描时高频词直接用字库索引引用，不随每趟扫描传输（软件打开瞬间就绪）。
static std::vector<std::wstring> g_libWords;
static long long g_nextId = 0;

// 当前扫描的可取消标志（nullptr = 没有在扫描）
static std::shared_ptr<std::atomic<bool>> g_scanCancel;
static volatile LONG g_scanRunning = 0;

struct ScanJobEx {
    std::wstring root;
    std::shared_ptr<std::atomic<bool>> cancel;
};

// 节点递归输出为 JSON。用 id 便于前端做展开态记录。
// 树很大时不递归输出全部（只输出到指定深度 + 每层上限），
// 页面按需再向宿主请求子节点 —— 避免一次传几百万节点撑爆内存。
static void NodeToJson(const ScanNode* n, int depth, int maxDepth, size_t kidsCap,
                       std::wstring& out, long long* idCounter) {
    long long id = ++(*idCounter);
    g_idMap[id] = const_cast<ScanNode*>(n);   // 记下映射，供 list 命令回查
    out += L"{";
    out += L"\"id\":" + std::to_wstring(id);
    out += L",\"name\":\"" + JsEscape(n->name) + L"\"";
    out += L",\"size\":" + std::to_wstring(n->size);
    out += L",\"fileCount\":" + std::to_wstring(n->fileCount);
    // 磁盘上的【真实】直接子项数 —— 页面用它判断 children 是否被截断（需要懒加载补全）
    out += L",\"dirCount\":" + std::to_wstring(n->children.size());
    out += n->isDir ? L",\"isDir\":true" : L",\"isDir\":false";
    out += L",\"path\":\"" + JsEscape(NodeFullPath(n)) + L"\"";
    out += L",\"hasParent\":" + std::wstring(n->parent ? L"true" : L"false");
    if (n->isDir && !n->children.empty() && depth < maxDepth) {
        // 子项按大小降序，最多 kidsCap 个
        std::vector<const ScanNode*> kids(n->children.begin(), n->children.end());
        std::stable_sort(kids.begin(), kids.end(),
                         [](const ScanNode* a, const ScanNode* b) {
                                 if (a->isDir != b->isDir) return a->isDir;
                                 return a->size > b->size; });
        if (kids.size() > kidsCap) kids.resize(kidsCap);
        out += L",\"children\":[";
        for (size_t i = 0; i < kids.size(); i++) {
            if (i) out += L",";
            NodeToJson(kids[i], depth + 1, maxDepth, kidsCap, out, idCounter);
        }
        out += L"]";
    }
    out += L"}";
}

// ---------- MFT / normal-walk dual scanner selection (impl after GetSetting) ----------
static int MftMode();
static bool IsDriveRootPath(const std::wstring& p);
static std::wstring ToVolumePath(const std::wstring& p);
// ============ 完整数据树存储 + 哈希去重 ============
// 每次扫描后把整棵树存成 JSON 到软件所在目录，文件名按模式区分：
//   MFT 直读 → full_mft.json ；普通遍历 → full_walk.json
// 序列化同时算 FNV-1a 64 哈希，与 manifest（fulldump_meta.txt）对比：
//   树没变 → 不重复写盘（跳过 IO）；树变了 → 写入并更新 manifest。
static std::string WtoUtf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
// JSON 字符串转义（含控制字符 → \uXXXX；JsEscape 只处理常用转义，完整树文件名可能含控制符）
static std::string JsonName(const std::wstring& w) {
    std::wstring esc;
    esc.reserve(w.size() + 16);
    for (wchar_t c : w) {
        switch (c) {
            case L'"': esc += L"\\\""; break;
            case L'\\': esc += L"\\\\"; break;
            case L'\n': esc += L"\\n"; break;
            case L'\r': esc += L"\\r"; break;
            case L'\t': esc += L"\\t"; break;
            case L'\b': esc += L"\\b"; break;
            case L'\f': esc += L"\\f"; break;
            default:
                if (c < 0x20) {
                    wchar_t b[8];
                    swprintf(b, 8, L"\\u%04x", (unsigned)c);
                    esc += b;
                } else esc.push_back(c);
        }
    }
    return WtoUtf8(esc);
}
// 流式序列化（显式栈迭代，不递归；边序列化边写，避免构造几百 MB 内存字符串）
// 返回 (FNV-1a64 哈希, 写入字节数)
static std::pair<ULONGLONG, size_t> SerializeFullTree(const ScanNode* root, FILE* fp) {
    ULONGLONG h = 1469598103934665603ULL;   // FNV offset basis
    std::string buf;
    buf.reserve(1 << 20);
    size_t total = 0;
    auto flush = [&]() {
        if (buf.empty()) return;
        fwrite(buf.data(), 1, buf.size(), fp);
        for (char c : buf) { h ^= (unsigned char)c; h *= 1099511628211ULL; }
        total += buf.size();
        buf.clear();
    };
    struct Fr { const ScanNode* n; size_t nextChild; bool opened; bool first; bool skipKids; };
    std::vector<Fr> st;
    st.reserve(256);
    st.push_back({ root, 0, false, true, false });
    ULONGLONG nodes = 0;
    std::unordered_set<const ScanNode*> visited;   // 防环：损坏记录可能形成 children 环
    while (!st.empty()) {
        Fr& f = st.back();
        const ScanNode* n = f.n;
        if (!f.opened) {
            f.opened = true;
            if (!f.first) buf += ",";
            buf += "{\"name\":\"";
            buf += JsonName(n->name);
            buf += "\",\"size\":";
            buf += std::to_string((long long)n->size);
            buf += n->isDir ? ",\"isDir\":true" : ",\"isDir\":false";
            nodes++;
            if ((nodes % 200000) == 0) LogLine(L"SERIAL progress nodes=" + std::to_wstring(nodes) + L" buf=" + std::to_wstring(buf.size()));
            if (n->isDir && !n->children.empty()) {
                if (visited.insert(n).second) buf += ",\"children\":[";
                else { f.skipKids = true; buf += "}"; }
            } else buf += "}";
            if (buf.size() >= (1 << 20)) flush();
            continue;
        }
        if (n->isDir && !f.skipKids && f.nextChild < n->children.size()) {
            const ScanNode* c = n->children[f.nextChild++];
            st.push_back({ c, 0, false, f.nextChild == 1, false });
            continue;
        }
        if (n->isDir && !n->children.empty() && !f.skipKids) buf += "]}";
        st.pop_back();
        if (buf.size() >= (1 << 20)) flush();
    }
    flush();
    LogLine(L"SERIAL nodes=" + std::to_wstring(nodes) + L" bytes=" + std::to_wstring(total));
    return { h, total };
}
static std::wstring MetaPath() { return ExeDir() + L"fulldump_meta.txt"; }
// 按文件前缀读上次哈希；0 = 没有记录
static ULONGLONG MetaGetHash(const std::wstring& file) {
    ULONGLONG last = 0;
    FILE* mf = nullptr;
    if (_wfopen_s(&mf, MetaPath().c_str(), L"r, ccs=UTF-8") == 0 && mf) {
        wchar_t line[1024];
        while (fgetws(line, 1024, mf)) {
            std::wstring w = line;
            if (w.compare(0, file.size(), file) != 0) continue;
            size_t sp = w.find(L' ', file.size());
            if (sp != std::wstring::npos)
                last = wcstoull(w.c_str() + sp + 1, nullptr, 10);
        }
        fclose(mf);
    }
    return last;
}
// 更新 manifest：重写全部（保留另一模式的行，只替换本文件那行）
static void MetaSet(const std::wstring& file, ULONGLONG hash, size_t bytes) {
    std::vector<std::wstring> keep;
    FILE* mf = nullptr;
    if (_wfopen_s(&mf, MetaPath().c_str(), L"r, ccs=UTF-8") == 0 && mf) {
        wchar_t line[1024];
        while (fgetws(line, 1024, mf)) {
            std::wstring w = line;
            while (!w.empty() && (w.back() == L'\n' || w.back() == L'\r')) w.pop_back();
            if (!w.empty() && w.compare(0, file.size(), file) != 0) keep.push_back(w);
        }
        fclose(mf);
    }
    keep.push_back(file + L" " + std::to_wstring(hash) + L" " + std::to_wstring(bytes));
    if (_wfopen_s(&mf, MetaPath().c_str(), L"w, ccs=UTF-8") == 0 && mf) {
        for (auto& k : keep) fwprintf(mf, L"%ls\n", k.c_str());
        fclose(mf);
    }
}
static void SaveFullTree(const ScanNode* root, const std::wstring& file) {
    if (!root) return;
    ULONGLONG lastHash = MetaGetHash(file);
    // 先写临时文件（避免中途失败损坏已有完整文件）
    std::wstring tmp = file + L".tmp";
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, (ExeDir() + tmp).c_str(), L"wb") != 0 || !fp) return;
    auto pr = SerializeFullTree(root, fp);
    fclose(fp);
    LogLine(L"FULLTREE " + file + L" hash=" + std::to_wstring(pr.first) +
            L" bytes=" + std::to_wstring(pr.second) +
            L" last=" + std::to_wstring(lastHash) +
            (lastHash == pr.first ? L" SAME=跳过写盘" : L" CHANGED"));
    if (lastHash == pr.first) {          // 与上次完全一致：不重复写盘
        DeleteFileW((ExeDir() + tmp).c_str());
        return;
    }
    MoveFileExW((ExeDir() + tmp).c_str(), (ExeDir() + file).c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    MetaSet(file, pr.first, pr.second);
    LogLine(L"FULLDUMP written " + file + L" bytes=" + std::to_wstring(pr.second));
}
// ================= 二进制全量传输（替代 JSON） =================
// 思路：不用 JSON。整棵树 → 二进制：① 字符串表（前缀树思路：收集去重 → 排序 →
// 相邻共享前缀增量编码，传输紧凑）；② 定长节点数组（28B/节点，父先子后）。
// 最后 base64 分块发给页面，前端 atob → 一次解析直接建树（无 JSON.parse）。
static std::string Base64Encode(const BYTE* d, size_t len) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        BYTE b0 = d[i];
        BYTE b1 = i + 1 < len ? d[i + 1] : 0;
        BYTE b2 = i + 2 < len ? d[i + 2] : 0;
        out += T[b0 >> 2];
        out += T[((b0 & 3) << 4) | (b1 >> 4)];
        out += (i + 1 < len) ? T[((b1 & 15) << 2) | (b2 >> 6)] : '=';
        out += (i + 2 < len) ? T[b2 & 63] : '=';
    }
    return out;
}
// 编码整棵树为二进制。内存：一次性峰值（order + 字符串表），用完即弃。
// ---------- 预字库（diskmate.lib） ----------
static std::wstring LibPath() { return ExeDir() + L"diskmate.lib"; }
static void LoadLibFile() {
    g_libWords.clear();
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, LibPath().c_str(), L"r, ccs=UTF-8") == 0 && fp) {
        wchar_t line[1024];
        while (fgetws(line, 1024, fp)) {
            std::wstring w = line;
            while (!w.empty() && (w.back() == L'\n' || w.back() == L'\r')) w.pop_back();
            if (!w.empty()) g_libWords.push_back(w);
        }
        fclose(fp);
    }
    LogLine(L"lib loaded: " + std::to_wstring(g_libWords.size()));
}
static void SaveLibFile(const std::vector<std::wstring>& words) {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, LibPath().c_str(), L"w, ccs=UTF-8") == 0 && fp) {
        for (const auto& w : words) fwprintf(fp, L"%ls\n", w.c_str());
        fclose(fp);
    }
}
// 扫描后更新字库：旧字库 + 本次词频>=2 的新词 → 字典序（索引稳定）→ 截断到上限 → 写回
static void UpdateLibFile(const std::vector<std::wstring>& names) {
    const size_t LIB_CAP = 4096;
    std::unordered_map<std::wstring, size_t> freq;
    for (const auto& nm : names) freq[nm]++;
    std::vector<std::pair<std::wstring, size_t>> tf(freq.begin(), freq.end());
    std::stable_sort(tf.begin(), tf.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });
    std::vector<std::wstring> merged = g_libWords;
    std::unordered_set<std::wstring> have(merged.begin(), merged.end());
    for (const auto& p : tf) {
        if (merged.size() >= LIB_CAP) break;
        if (p.second >= 2 && !have.count(p.first)) {
            merged.push_back(p.first);
            have.insert(p.first);
        }
    }
    std::sort(merged.begin(), merged.end());
    if (merged.size() > LIB_CAP) merged.resize(LIB_CAP);
    SaveLibFile(merged);
    LogLine(L"lib updated: " + std::to_wstring(merged.size()));
}
static bool EncodeTreeBinary(ScanResult& tree, std::vector<BYTE>& out) {
    // 1. DFS 收集节点（父先子后，保证父位置 < 子位置）+ 名字。
    //    子项必须【目录优先 + size 降序】—— 前端 sizedItems 不再排序，
    //    布局顺序由后端保证（squarify 输入乱序 → 大量细长条，踩过）。
    std::vector<ScanNode*> order;
    order.reserve(4 * 1024 * 1024);
    std::vector<std::wstring> names;
    names.reserve(4 * 1024 * 1024);
    std::function<void(ScanNode*)> dfs = [&](ScanNode* n) {
        order.push_back(n);
        names.push_back(n->name);
        std::vector<ScanNode*> kids(n->children.begin(), n->children.end());
        std::stable_sort(kids.begin(), kids.end(),
                         [](const ScanNode* a, const ScanNode* b) {
                             if (a->isDir != b->isDir) return a->isDir;
                             return a->size > b->size;
                         });
        for (ScanNode* c : kids) dfs(c);
    };
    dfs(tree.root);
    // 词频统计（预字库用）：输出 top 200 高频名字到日志
    {
        std::unordered_map<std::wstring, size_t> freq;
        for (const auto& nm : names) freq[nm]++;
        std::vector<std::pair<std::wstring, size_t>> tf(freq.begin(), freq.end());
        std::stable_sort(tf.begin(), tf.end(),
                         [](const auto& a, const auto& b) { return a.second > b.second; });
        std::wstring line = L"STRFREQ total=" + std::to_wstring(freq.size());
        for (size_t i = 0; i < tf.size() && i < 200; i++) {
            line += L" | " + tf[i].first + L"(" + std::to_wstring(tf[i].second) + L")";
        }
        LogLine(line);
    }
    const size_t nodeCount = order.size();
    if (nodeCount == 0) return false;
    // 2. 字符串表：预字库优先（高频词引用字库索引，不随扫描传输），
    //    其余字符串 → 去重 → 排序 → 前缀增量编码（排序后相邻共享前缀最多，等于 trie DFS 输出）
    const size_t libN = g_libWords.size();
    std::unordered_set<std::wstring> libSet(g_libWords.begin(), g_libWords.end());
    std::unordered_set<std::wstring> set;
    for (const auto& nm : names) if (!libSet.count(nm)) set.insert(nm);
    std::vector<std::wstring> strs(set.begin(), set.end());
    std::sort(strs.begin(), strs.end());
    const size_t wordCount = strs.size();
    std::unordered_map<std::wstring, DWORD> idx;
    idx.reserve(wordCount * 2);
    for (size_t i = 0; i < wordCount; i++) idx[strs[i]] = (DWORD)i;
    std::vector<BYTE> inc;   // 增量流：u16 commonLen(码元) + u16 suffixLen(码元) + UTF-16LE suffix 字节
    inc.reserve(16 * 1024 * 1024);
    std::wstring prev;
    for (size_t i = 0; i < wordCount; i++) {
        const std::wstring& w = strs[i];
        size_t common = 0;
        size_t lim = prev.size() < w.size() ? prev.size() : w.size();
        while (common < lim && prev[common] == w[common]) common++;
        DWORD cl = (DWORD)common, sl = (DWORD)(w.size() - common);
        inc.push_back((BYTE)(cl & 0xFF)); inc.push_back((BYTE)((cl >> 8) & 0xFF));
        inc.push_back((BYTE)(sl & 0xFF)); inc.push_back((BYTE)((sl >> 8) & 0xFF));
        // suffix 的 UTF-16LE 字节（码元 ×2）
        for (size_t k = common; k < w.size(); k++) {
            wchar_t ch = w[k];
            inc.push_back((BYTE)(ch & 0xFF));
            inc.push_back((BYTE)((ch >> 8) & 0xFF));
        }
        prev = w;
    }
    // 3. 组装：header(28B) + 增量流 + 节点区
    out.clear();
    out.reserve(40 * 1024 * 1024);
    auto putU32 = [&](DWORD v) {
        out.push_back((BYTE)(v & 0xFF)); out.push_back((BYTE)((v >> 8) & 0xFF));
        out.push_back((BYTE)((v >> 16) & 0xFF)); out.push_back((BYTE)((v >> 24) & 0xFF));
    };
    putU32(0x4B4D4442);      // magic "BDMK"
    putU32(3);               // version（v3：预字库 + 词表双段）
    putU32((DWORD)nodeCount);
    putU32((DWORD)libN);     // libCount：前端缓存的字库词数
    putU32((DWORD)wordCount);
    putU32((DWORD)inc.size());
    putU32(0);               // reserved
    out.insert(out.end(), inc.begin(), inc.end());
    // 节点区：28B/节点 = pid(4) + size(8) + fileCount(4) + dirCount(4) + flags(4) + nameIdx(4)
    std::unordered_map<ScanNode*, DWORD> pos;
    pos.reserve(nodeCount * 2);
    for (size_t i = 0; i < nodeCount; i++) {
        ScanNode* n = order[i];
        DWORD pid = n->parent ? pos[n->parent] : 0xFFFFFFFF;
        pos[n] = (DWORD)i;
        // nameIdx < libN → 字库词；否则 → 词表词（索引 - libN）
        DWORD nameId;
        auto lit = std::lower_bound(g_libWords.begin(), g_libWords.end(), n->name);
        if (lit != g_libWords.end() && *lit == n->name)
            nameId = (DWORD)(lit - g_libWords.begin());
        else
            nameId = (DWORD)(libN + idx[n->name]);
        putU32(pid);
        ULONGLONG sz = n->size;
        for (int k = 0; k < 8; k++) out.push_back((BYTE)((sz >> (k * 8)) & 0xFF));
        putU32(n->fileCount);
        putU32((DWORD)n->children.size());
        putU32(n->isDir ? 1u : 0u);
        putU32(nameId);
    }
    // 扫描后更新预字库（合并本次高频词，下次启动生效）
    UpdateLibFile(names);
    return true;
}
// base64 分块发送（每块 ~512KB base64 字符）；结束后发 binDone（带 mode/stats）
static void SendBinaryTree(ScanResult& tree, int mode, const ScanStats& stats) {
    std::vector<BYTE> bin;
    if (!EncodeTreeBinary(tree, bin)) {
        LogLine(L"BIN encode failed");
        return;
    }
    std::string b64 = Base64Encode(bin.data(), bin.size());
    const size_t CHUNK = 512 * 1024;
    size_t seq = 0;
    for (size_t p = 0; p < b64.size(); p += CHUNK, seq++) {
        std::string part = b64.substr(p, std::min(CHUNK, b64.size() - p));
        // base64 只含 A-Za-z0-9+/=，JSON 安全，无需转义
        std::wstring msg = L"{\"kind\":\"binPart\",\"seq\":" + std::to_wstring(seq) +
                           L",\"size\":" + std::to_wstring(part.size()) +
                           L",\"b64\":\"" + std::wstring(part.begin(), part.end()) + L"\"}";
        auto* payload = new std::wstring(msg);
        if (!PostMessageW(g_hwnd, WM_APP_WEBJSON, 0, (LPARAM)payload)) delete payload;
    }
    std::wstring done = L"{\"kind\":\"binDone\",\"mode\":" + std::to_wstring(mode) +
        L",\"stats\":{\"items\":" + std::to_wstring(stats.items) +
        L",\"bytes\":" + std::to_wstring(stats.bytes) +
        L",\"skipped\":" + std::to_wstring(stats.skipped) + L"}}";
    auto* payload = new std::wstring(done);
    if (!PostMessageW(g_hwnd, WM_APP_WEBJSON, 0, (LPARAM)payload)) delete payload;
    LogLine(L"BIN sent bin=" + std::to_wstring(bin.size()) +
            L" b64=" + std::to_wstring(b64.size()) +
            L" chunks=" + std::to_wstring(seq));
}
static DWORD WINAPI ScanThreadProc(LPVOID param) {
    ScanJobEx* job = (ScanJobEx*)param;
    ScanStats stats;
    auto progress = [&](ULONGLONG items, ULONGLONG bytes, ULONGLONG skipped) {
        (void)skipped;
        // 只更新最新值 + 必要时投递一个「有进度」的唤醒消息（不分配堆内存）
        g_progItems = items;
        g_progBytes = bytes;
        if (InterlockedExchange(&g_progPending, 1) == 0)
            PostMessageW(g_hwnd, WM_APP_PROGRESS, 0, 0);
    };
    // 用设置里的扫描参数（线程数 1-16 / 跳过隐藏 / 跟随重解析点）
    AppSettings& cfg = S();
    ScanResult res;
    bool usedMft = false;
    // MFT direct read: try only for drive-root paths; auto-fallback to walk on open/parse failure
    if (MftMode() != 0 && !job->cancel->load()) {
        std::wstring vol = ToVolumePath(job->root);
        if (!vol.empty()) {
            ScanStats mftStats;
            ScanResult mr = ScanTreeMFT(vol, *job->cancel, &mftStats, progress,
                                        cfg.threads, cfg.skipHidden, cfg.followReparse);
            if (mr.root) {
                res = std::move(mr);
                stats = mftStats;
                usedMft = true;
                LogLine(L"SCAN MODE=MFT vol=" + vol);
            } else {
                LogLine(L"MFT scan failed/unavailable, fallback to directory walk");
            }
        }
    }
    if (!usedMft && !job->cancel->load()) {
        res = ScanTree(job->root, *job->cancel, &stats, progress,
                       cfg.threads, cfg.skipHidden, cfg.followReparse);
    }
    LogLine(L"SCAN MODE=" + std::wstring(usedMft ? L"MFT" : L"walk"));
    {
        // 保留这棵树（页面按需查询子目录要用）；旧树在此释放
        g_tree = std::move(res);
    }
    ScanResult& resRef = g_tree;

    // 全量传输（不截断、不懒加载）：扫描结果整棵树分批发给页面
    int actualMode = usedMft ? MftMode() : 0;
    LogLine(L"ScanTree returned root=" + std::to_wstring(resRef.root != nullptr));
    if (resRef.root) {
        // 二进制全量传输：字符串表（前缀树式去重+排序共享前缀增量编码）+ 定长节点数组。
        // 不用 JSON：前端 atob → 一次解析直接建树，无 JSON.parse。
        SendBinaryTree(resRef, actualMode, stats);
    } else {
        std::wstring done = L"{\"kind\":\"treeDone\",\"mode\":" +
            std::to_wstring(actualMode) + L",\"stats\":{\"items\":0,\"bytes\":0,\"skipped\":0}}";
        auto* payload = new std::wstring(done);
        if (!PostMessageW(g_hwnd, WM_APP_WEBJSON, 0, (LPARAM)payload)) delete payload;
    }

    LogLine(L"SCAN DONE items=" + std::to_wstring(stats.items));

    // 被取消的扫描：丢弃结果（否则会把旧目录的数据推给页面，覆盖新扫描）
    if (job->cancel->load()) {
        LogLine(L"SCAN cancelled, result discarded");
        InterlockedExchange(&g_scanRunning, 0);
        delete job;
        return 0;
    }

    // 完整树 dump（诊断；文件名按模式区分，不互相覆盖）—— 仅加 --dumpfulltree 时才写盘
    if (g_dumpFullTree)
        SaveFullTree(resRef.root, (MftMode() != 0) ? L"full_mft.json" : L"full_walk.json");
    InterlockedExchange(&g_scanRunning, 0);
    delete job;
    return 0;
}

static void StartScan(const std::wstring& rawRoot) {
    std::wstring root = NormalizeRoot(rawRoot);

    // 已在扫描：先取消上一次并等它退出，避免两个扫描线程同时跑
    // （两个线程会各自建树、各自推 JSON，页面被交错的结果覆盖 → 界面错乱）
    if (InterlockedCompareExchange(&g_scanRunning, 1, 0) != 0) {
        LogLine(L"StartScan ignored: already scanning");
        // 通知页面：这次没扫起来，把按钮/进度条复位
        std::wstring* p = new std::wstring(
            L"{\"kind\":\"scanfail\",\"reason\":\"已有扫描在进行中\"}");
        if (!PostMessageW(g_hwnd, WM_APP_WEBJSON, 0, (LPARAM)p)) delete p;
        return;
    }
    if (g_scanCancel) g_scanCancel->store(true);

    auto cancel = std::make_shared<std::atomic<bool>>(false);
    g_scanCancel = cancel;
    g_idMap.clear();          // 新扫描：旧树的 id 映射作废
    g_nextId = 0;
    auto* job = new ScanJobEx{ root, cancel };
    HANDLE h = CreateThread(nullptr, 0, ScanThreadProc, job, 0, nullptr);
    if (h)
        CloseHandle(h);
    else {
        InterlockedExchange(&g_scanRunning, 0);
        delete job;
    }
}


// ---------- 极简设置持久化 ----------
// 存 exe 同目录 diskmate.ini，key=value 每行一个。
// 注意：必须「读全部 → 改一项 → 写全部」，否则写一项会清掉其它项。
static std::wstring SettingsPath() { return ExeDir() + L"diskmate.ini"; }

static std::vector<std::pair<std::wstring, std::wstring>> LoadSettings() {
    std::vector<std::pair<std::wstring, std::wstring>> kv;
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, SettingsPath().c_str(), L"r, ccs=UTF-8") == 0 && fp) {
        wchar_t line[1024];
        while (fgetws(line, 1024, fp)) {
            std::wstring w = line;
            while (!w.empty() && (w.back() == L'\n' || w.back() == L'\r')) w.pop_back();
            size_t eq = w.find(L'=');
            if (eq == std::wstring::npos || eq == 0) continue;
            std::wstring k = w.substr(0, eq), v = w.substr(eq + 1);
            while (!v.empty() && (v.back() == L'	')) v.pop_back();
            kv.emplace_back(k, v);
        }
        fclose(fp);
    }

    return kv;
}

static std::wstring GetSetting(const wchar_t* key, const std::wstring& def) {
    for (auto& p : LoadSettings())
        if (p.first == key) return p.second;
    return def;
}

static void SetSetting(const wchar_t* key, const std::wstring& value) {
    auto kv = LoadSettings();
    bool found = false;
    for (auto& p : kv) {
        if (p.first == key) { p.second = value; found = true; break; }
    }

    if (!found) kv.emplace_back(key, value);
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, SettingsPath().c_str(), L"w, ccs=UTF-8") == 0 && fp) {
        for (auto& p : kv)
            fwprintf(fp, L"%ls=%ls\n", p.first.c_str(), p.second.c_str());
        fclose(fp);
    }
}

// Scan mode: diskmate.ini key "mft" -- 0=walk only, 1=MFT only (fallback to walk), 2=auto (default, MFT for drive roots)
static int MftMode() {
    std::wstring v = GetSetting(L"mft", L"2");
    int m = _wtoi(v.c_str());
    return (m == 0 || m == 1) ? m : 2;
}
static bool IsDriveRootPath(const std::wstring& p) {
    // 仅盘符根（"D:" 或 "D:\"）：子目录路径不启用 MFT（MFT 只能整卷读）
    if (p.size() < 2 || !iswalpha(p[0]) || p[1] != L':' ) return false;
    for (size_t i = 2; i < p.size(); i++)
        if (p[i] != L'\\' && p[i] != L'/') return false;
    return true;
}
static std::wstring ToVolumePath(const std::wstring& p) {
    if (IsDriveRootPath(p))
        return L"\\\\.\\" + std::wstring(1, p[0]) + L":";
    return L"";
}
static bool IsElevated() {
    HANDLE hProc = GetCurrentProcess();
    HANDLE hTok = nullptr;
    if (!OpenProcessToken(hProc, TOKEN_QUERY, &hTok)) return false;
    TOKEN_ELEVATION te{};
    DWORD sz = 0;
    bool ok = GetTokenInformation(hTok, TokenElevation, &te, sizeof(te), &sz) && te.TokenIsElevated != 0;
    CloseHandle(hTok);
    return ok;
}

// ---------- 通用界面偏好存储 ----------
// C++ 只负责「把一个字符串存下来/读回去」，【不关心内容】。
// 界面自己把偏好序列化成 JSON 字符串（schema 由 JS 拥有）。
// 这样以后加任何界面设置都不用再改 C++ —— 宿主是通用通道，不是设置中心。
static std::wstring PrefsPath() { return ExeDir() + L"ui-prefs.json"; }

static std::wstring LoadPrefsBlob() {
    std::wstring out;
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, PrefsPath().c_str(), L"rb") == 0 && fp) {
        fseek(fp, 0, SEEK_END);
        long n = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (n > 0) {
            std::string buf(n, '\0');
            fread(&buf[0], 1, n, fp);
            // UTF-8 → UTF-16
            int wl = MultiByteToWideChar(CP_UTF8, 0, buf.c_str(), (int)buf.size(), nullptr, 0);
            out.resize(wl);
            if (wl) MultiByteToWideChar(CP_UTF8, 0, buf.c_str(), (int)buf.size(), &out[0], wl);
        }
        fclose(fp);
    }
    return out;
}

static void SavePrefsBlob(const std::wstring& json) {
    int n = WideCharToMultiByte(CP_UTF8, 0, json.c_str(), (int)json.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string buf(n, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, json.c_str(), (int)json.size(),
                               &buf[0], n, nullptr, nullptr);
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, PrefsPath().c_str(), L"wb") == 0 && fp) {
        fwrite(buf.data(), 1, buf.size(), fp);
        fclose(fp);
    }
}

// ---------- 页面命令分发 ----------
// 自定义菜单（当设置里 shellMenu=false 时用）：按 MenuConfig 的开关显示项，
// 命令自己实现（打开/在资源管理器显示/属性/复制路径…），不依赖 shell 扩展。
enum {
    CM_OPEN = 1, CM_EXPLORE, CM_TERMINAL, CM_PROPS,
    CM_COPYPATH, CM_COPYNAME, CM_COPYSIZE,
    CM_ENTER, CM_UP, CM_BACK, CM_FORWARD, CM_RESCAN,
    CM_DELRECYCLE, CM_DELFOREVER
};

static void SetClipboardText(HWND hwnd, const std::wstring& text) {
    if (!OpenClipboard(hwnd)) return;
    EmptyClipboard();
    size_t cb = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, cb);
    if (h) {
        memcpy(GlobalLock(h), text.c_str(), cb);
        GlobalUnlock(h);
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
}

// 返回：是否已处理（true 表示这是自定义菜单路径）
static bool ShowCustomContextMenu(HWND hwnd, const std::vector<std::wstring>& paths) {
    if (paths.empty()) return false;
    AppSettings& c = S();
    MenuConfig& m = c.menu;
    const std::wstring& path = paths[0];
    bool multi = paths.size() > 1;

    HMENU menu = CreatePopupMenu();
    if (!menu) return false;

    auto add = [&](bool on, UINT id, const wchar_t* text) {
        if (on) AppendMenuW(menu, MF_STRING, id, text);
    };
    if (m.open)         AppendMenuW(menu, MF_STRING, CM_OPEN, multi ? L"打开（多项）" : L"打开");
    add(m.explore,      CM_EXPLORE,  L"在资源管理器中显示");
    add(m.terminal,     CM_TERMINAL, L"在此处打开终端");
    add(m.props,        CM_PROPS,    L"属性");
    if (m.open || m.explore || m.terminal || m.props) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(m.copyPath,     CM_COPYPATH, L"复制完整路径");
    add(m.copyName,     CM_COPYNAME, L"复制文件名");
    add(m.copySize,     CM_COPYSIZE, L"复制大小");
    if (m.copyPath || m.copyName || m.copySize) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(m.enter,        CM_ENTER,    L"进入此目录");
    add(m.back,         CM_BACK,     L"后退");
    add(m.forward,      CM_FORWARD,  L"前进");
    add(m.rescan,       CM_RESCAN,   L"重新扫描此项");
    if (m.deleteRecycle || m.deleteForever) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(m.deleteRecycle, CM_DELRECYCLE, L"删除到回收站");
    add(m.deleteForever, CM_DELFOREVER, L"永久删除");

    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    UINT sel = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN,
                              pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    if (!sel) return true;      // 已处理（用户取消）

    // 命令分发
    switch (sel) {
    case CM_OPEN:
    case CM_ENTER: {
        // 多项时逐项打开
        for (const auto& x : paths)
            ShellExecuteW(hwnd, L"open", x.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
    case CM_EXPLORE:
        ShellExecuteW(hwnd, L"open", L"explorer.exe",
                      (L"/select,\"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        break;
    case CM_TERMINAL: {
        // 在文件所在目录开一个 cmd
        std::wstring dir = path;
        size_t bs = dir.find_last_of(L"\\/");
        if (bs != std::wstring::npos) dir = dir.substr(0, bs);
        std::wstring a = L"/K cd /d \"" + dir + L"\"";
        ShellExecuteW(hwnd, L"open", L"cmd.exe", a.c_str(), dir.c_str(), SW_SHOWNORMAL);
        break;
    }
    case CM_PROPS:
        ShellExecuteW(hwnd, L"properties", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case CM_COPYPATH: SetClipboardText(hwnd, path); break;
    case CM_COPYNAME: {
        size_t bs = path.find_last_of(L"\\/");
        SetClipboardText(hwnd, bs == std::wstring::npos ? path : path.substr(bs + 1));
        break;
    }
    case CM_COPYSIZE: {
        // 交给页面显示更合适；这里只放个占位（大小从树上取）
        SetClipboardText(hwnd, path);
        break;
    }
    case CM_UP:    EvalJs(L"document.getElementById('up').click();"); break;
    case CM_BACK:  EvalJs(L"document.getElementById('back').click();"); break;
    case CM_FORWARD: EvalJs(L"document.getElementById('forward')&&document.getElementById('forward').click();"); break;
    case CM_RESCAN: EvalJs(L"document.getElementById('scan').click();"); break;
    case CM_DELRECYCLE:
    case CM_DELFOREVER: {
        bool perm = (sel == CM_DELFOREVER);
        std::wstring args;
        for (const auto& x : paths) { if (!args.empty()) args += L" "; args += L"\"" + x + L"\""; }
        if (!S().confirmDelete && !perm) {
            // 不确认：直接调用（仍走系统，可撤销）
            ShellExecuteW(hwnd, L"open", L"cmd.exe",
                          (L"/C choice /C Y /T 0 /D Y >nul & del /F /Q /S " + args).c_str(),
                          nullptr, SW_HIDE);
        } else {
            MessageBoxW(hwnd, L"删除到回收站在 Web 版尚未接入（下一步用 IFileOperation 实现）。",
                        L"DiskMate", MB_ICONINFORMATION);
        }
        break;
    }
    }
    return true;
}



// ---------- 系统「壳菜单」：让右键菜单与资源管理器完全一致 ----------
// 用 IShellFolder + IContextMenu 取系统的上下文菜单：
//   菜单项 = 资源管理器那一套（打开方式、7-Zip、Git…全部 shell 扩展都在）
//   命令   = 由系统执行（重命名/删除/属性等不需要自己实现）
// 参考：MSDN「How to Invoke a Verb on a Shell Item」/ IContextMenu 用法。
// 在独立 STA 线程里做壳菜单的全部 COM 调用。
// 原因：某些 shell 扩展在 QueryContextMenu 内部会长时间阻塞（同步盘/杀毒），
// 在 UI 线程里同步调用会把界面冻死（踩过：卡在 [5]）。独立线程 + 自己的消息泵
// 就不会影响主界面。
static bool ShowShellContextMenu(HWND hwnd, const std::wstring& path);
static bool ShowShellContextMenuMulti(HWND hwnd, const std::vector<std::wstring>& paths);

static LRESULT CALLBACK ShellMenuWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcW(h, m, w, l);
}

static DWORD WINAPI ShellMenuThread(LPVOID param) {
    std::vector<std::wstring>* pp = (std::vector<std::wstring>*)param;
    std::vector<std::wstring> paths = *pp;
    delete pp;
    std::wstring path = paths.empty() ? L"" : paths[0];

    // TrackPopupMenu 有个硬约束：必须在【拥有窗口的线程】上调用，否则菜单
    // 建好了也不显示（踩过：items=18、sel=0、屏幕上没东西）。
    // 所以本线程自建一个隐藏窗口，用它当 owner。
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ShellMenuWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DiskMateShellMenuHost";
    RegisterClassExW(&wc);      // 重复注册返回 0，无妨
    HWND owner = CreateWindowExW(0, L"DiskMateShellMenuHost", L"", WS_POPUP,
                                 0, 0, 0, 0, nullptr, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
    LogLine(L"shellmenu[0] owner=" + std::to_wstring((uintptr_t)owner) +
            L" tid=" + std::to_wstring(GetCurrentThreadId()));

    // MTA：不需要消息泵，跨单元 COM 调用不会卡（STA 会卡在 QueryContextMenu）。
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    LogLine(L"shellmenu[0b] CoInitializeEx(MTA) hr=0x" +
            std::to_wstring((unsigned long)co));

    ShowShellContextMenuMulti(owner ? owner : g_hwnd, paths);

    CoUninitialize();
    if (owner) DestroyWindow(owner);
    return 0;
}

// 多选版：把 N 个路径变成 N 个 child PIDL，交给同一个 IContextMenu。
// IContextMenu 要求所有项同一父目录（资源管理器也这样）；不同父目录时只取同目录的那些。
static bool ShowShellContextMenuMulti(HWND hwnd, const std::vector<std::wstring>& paths) {
    if (paths.empty()) return false;
    if (paths.size() == 1) return ShowShellContextMenu(hwnd, paths[0]);
    LogLine(L"shellmenu[M1] enter n=" + std::to_wstring(paths.size()));
    for (size_t k = 0; k < paths.size(); k++)
        LogLine(L"shellmenu[M1." + std::to_wstring(k) + L"] " + paths[k]);

    std::vector<PIDLIST_ABSOLUTE> pidls;
    for (const auto& pfx : paths) {
        PIDLIST_ABSOLUTE p = nullptr;
        SFGAOF a = 0;
        if (SUCCEEDED(SHParseDisplayName(pfx.c_str(), nullptr, &p, 0, &a)) && p)
            pidls.push_back(p);
    }
    if (pidls.empty()) return false;
    if (pidls.size() == 1) { CoTaskMemFree(pidls[0]); return ShowShellContextMenu(hwnd, paths[0]); }

    // 用父目录的【路径字符串】判断是否同目录 —— 不能用 IShellFolder 指针比较：
    // SHBindToParent 对同一目录每次返回的接口实例可能不同，== 恒为假（踩过：
    // 同目录 5 个文件却 sameParentCount=1）。
    std::vector<PIDLIST_ABSOLUTE> parents;      // 每个项的父目录 PIDL（用于比较）
    auto parentKey = [](PIDLIST_ABSOLUTE pidl) -> std::wstring {
        PIDLIST_ABSOLUTE p2 = ILClone(pidl);
        if (!p2) return L"";
        ILRemoveLastID(p2);
        wchar_t buf[MAX_PATH * 2] = {};
        SHGetPathFromIDListW(p2, buf);
        CoTaskMemFree(p2);
        return buf;
    };

    std::wstring key0 = parentKey(pidls[0]);
    IShellFolder* parent = nullptr;
    PCUITEMID_CHILD child0 = nullptr;
    if (FAILED(SHBindToParent(pidls[0], IID_IShellFolder, (void**)&parent, &child0)) || !parent) {
        for (auto x : pidls) CoTaskMemFree(x);
        return false;
    }
    std::vector<PCUITEMID_CHILD> children;
    children.push_back(child0);
    for (size_t i = 1; i < pidls.size(); i++) {
        if (parentKey(pidls[i]) != key0) continue;      // 不同目录 → 跳过
        IShellFolder* par2 = nullptr;
        PCUITEMID_CHILD c2 = nullptr;
        if (SUCCEEDED(SHBindToParent(pidls[i], IID_IShellFolder, (void**)&par2, &c2)) && par2) {
            children.push_back(c2);
            par2->Release();
        }
    }
    LogLine(L"shellmenu[M2] sameParentCount=" + std::to_wstring(children.size()));

    bool ok = false;
    IContextMenu* cm = nullptr;
    HRESULT hr = parent->GetUIObjectOf(hwnd, (UINT)children.size(), children.data(),
                                       IID_IContextMenu, nullptr, (void**)&cm);
    LogLine(L"shellmenu[M3] GetUIObjectOf hr=0x" + std::to_wstring((unsigned long)hr)
            + L" cm=" + (cm ? L"ok" : L"null"));
    if (SUCCEEDED(hr) && cm) {
        HMENU menu = CreatePopupMenu();
        if (menu) {
            const UINT ID_FIRST = 0x8000, ID_LAST = 0x8FFF;
            hr = cm->QueryContextMenu(menu, 0, ID_FIRST, ID_LAST, CMF_NORMAL);
            LogLine(L"shellmenu[M4] QueryContextMenu hr=0x" + std::to_wstring((unsigned long)hr)
                    + L" items=" + std::to_wstring(GetMenuItemCount(menu)));
            if (SUCCEEDED(hr)) {
                POINT pt; GetCursorPos(&pt);
                MSG msg;
                PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
                SetForegroundWindow(hwnd);
                UINT sel = TrackPopupMenu(menu,
                    TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN,
                    pt.x, pt.y, 0, hwnd, nullptr);
                LogLine(L"shellmenu[M5] sel=" + std::to_wstring(sel));
                if (sel) {
                    CMINVOKECOMMANDINFOEX info = {};
                    info.cbSize = sizeof(info);
                    info.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
                    info.hwnd = hwnd;
                    info.lpVerbW = MAKEINTRESOURCEW(sel - ID_FIRST);
                    info.lpVerb  = MAKEINTRESOURCEA(sel - ID_FIRST);
                    info.nShow = SW_SHOWNORMAL;
                    info.ptInvoke = pt;
                    HRESULT hr2 = cm->InvokeCommand((LPCMINVOKECOMMANDINFO)&info);
                    LogLine(L"shellmenu[M6] InvokeCommand hr=0x" + std::to_wstring((unsigned long)hr2));
                    ok = true;
                }
            }
            DestroyMenu(menu);
        }
        cm->Release();
    }
    parent->Release();
    for (auto x : pidls) CoTaskMemFree(x);
    return ok;
}


static void OnMessageFromPage(const std::wstring& json) {
    LogLine(L"MSG: " + json.substr(0, 120));
    EvalJs(L"window.__host.__hostEcho(" + json + L");");
    // 极简解析：{"cmd":"scan","path":"D:\\x"}
    auto getStr = [&](const wchar_t* key) -> std::wstring {
        std::wstring pat = std::wstring(L"\"") + key + L"\"";
        size_t p = json.find(pat);
        if (p == std::wstring::npos) return L"";
        p = json.find(L':', p);
        if (p == std::wstring::npos) return L"";
        p = json.find(L'"', p);
        if (p == std::wstring::npos) return L"";
        std::wstring out;
        for (size_t i = p + 1; i < json.size(); i++) {
            wchar_t c = json[i];
            if (c == L'\\' && i + 1 < json.size()) {
                wchar_t nx = json[++i];
                switch (nx) {
                    case L'n': out.push_back(L'\n'); break;
                    case L't': out.push_back(L'\t'); break;
                    case L'r': out.push_back(L'\r'); break;
                    default: out.push_back(nx); break;
                }
                continue;
            }
            if (c == L'"') break;
            out.push_back(c);
        }
        return out;
    };
    std::wstring cmd = getStr(L"cmd");
    LogLine(L"CMD=" + cmd + L" PATH=" + getStr(L"path"));
    if (cmd == L"scan") {
        // 记住这次扫描的路径（下次启动自动填回输入框）
        {
            std::wstring rp = NormalizeRoot(getStr(L"path"));
            if (!rp.empty() && IsDirPath(rp)) {
                SetSetting(L"lastPath", rp);
                LogLine(L"saved lastPath=" + rp);
            }
        }
        // 【不要】在这里用 EvalJs 改页面 UI —— UI 状态归页面自己管。
        // 宿主只负责：接受扫描，或者回一条 scanfail 让页面恢复。
        {
            std::wstring r = NormalizeRoot(getStr(L"path"));
            LogLine(L"StartScan root=" + r + L" isDir=" + std::to_wstring(IsDirPath(r)));
            if (!IsDirPath(r)) {
                // 无效路径：通知页面复位（否则页面永远停在"扫描中"）
                std::wstring* p = new std::wstring(
                    L"{\"kind\":\"scanfail\",\"reason\":\"路径无效\"}");
                if (!PostMessageW(g_hwnd, WM_APP_WEBJSON, 0, (LPARAM)p)) delete p;
            } else {
                StartScan(r);
            }
        }
    } else if (cmd == L"stop") {
        // 停止扫描：置取消标志，扫描线程会尽快退出并丢弃结果。
        // 注意：不要在这里调 setProgress —— 那会把页面按钮又切回「停止」，
        // 看起来像"点停止没用"。UI 状态交给页面自己管（页面已经处理了）。
        if (g_scanCancel) g_scanCancel->store(true);
        InterlockedExchange(&g_scanRunning, 0);
        LogLine(L"STOP requested");
    } else if (cmd == L"diag") {
        // 页面导出的 PNG（data:image/png;base64,...）写到 exe 同目录。
        // what=png → tm_dump.png；what=glpng → gl_layer.png；what=d2png → d2_layer.png
        const wchar_t* pngName = nullptr;
        if      (json.find(L"\"what\":\"glpng\"") != std::wstring::npos) pngName = L"gl_layer.png";
        else if (json.find(L"\"what\":\"d2png\"") != std::wstring::npos) pngName = L"d2_layer.png";
        else if (json.find(L"\"what\":\"png\"") != std::wstring::npos)    pngName = L"tm_dump.png";
        if (pngName) {
            size_t p = json.find(L"base64,");
            if (p != std::wstring::npos) {
                std::wstring b64 = json.substr(p + 7);
                // 去掉结尾的 "}
                while (!b64.empty() && (b64.back() == L'"' || b64.back() == L'}')) b64.pop_back();
                static const wchar_t* TBL =
                    L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                std::string out;
                int val = 0, bits = 0;
                for (wchar_t ch : b64) {
                    const wchar_t* f = wcschr(TBL, ch);
                    if (!f) continue;
                    val = (val << 6) | (int)(f - TBL);
                    bits += 6;
                    if (bits >= 8) { bits -= 8; out.push_back((char)((val >> bits) & 0xFF)); }
                }
                std::wstring path = ExeDir() + pngName;
                FILE* fp = nullptr;
                if (_wfopen_s(&fp, path.c_str(), L"wb") == 0 && fp) {
                    fwrite(out.data(), 1, out.size(), fp);
                    fclose(fp);
                    LogLine(std::wstring(L"PNG ") + pngName + L" bytes=" + std::to_wstring(out.size()));
                }
            }
        }
        LogLine(L"DIAG " + json.substr(0, 700));
    } else if (cmd == L"menu") {
        // 右键菜单：调用系统「壳菜单」（IShellFolder + IContextMenu）。
        // 必须在【消息循环】里执行，不能在 WebMessage 回调里同步调用：
        // 某些 shell 扩展（如同步盘/杀毒）在 QueryContextMenu 内部会做跨 STA 调用，
        // 需要消息泵 —— 回调栈里没有消息循环 → 死锁（踩过：卡在 [5]，界面无响应）。
        std::vector<std::wstring> all;
        {
            size_t ap = json.find(L"\"paths\":[");
            if (ap != std::wstring::npos) {
                size_t i = json.find(L'[', ap) + 1;
                while (i < json.size()) {
                    while (i < json.size() && (json[i] == L' ' || json[i] == L',')) i++;
                    if (i >= json.size() || json[i] == L']') break;
                    if (json[i] != L'"') break;
                    std::wstring one;
                    for (i++; i < json.size(); i++) {
                        wchar_t c = json[i];
                        if (c == L'\\' && i + 1 < json.size()) {
                            wchar_t nx = json[++i];
                            if (nx == L'n') one.push_back(L'\n');
                            else if (nx == L't') one.push_back(L'\t');
                            else if (nx == L'r') one.push_back(L'\r');
                            else one.push_back(nx);
                        } else if (c == L'"') { i++; break; }
                        else one.push_back(c);
                    }
                    if (!one.empty()) all.push_back(one);
                }
            }
        }
        if (all.empty()) {
            std::wstring p = getStr(L"path");
            if (!p.empty()) all.push_back(p);
        }
        // 相对路径 → 绝对（MFT 直读根名可能带 .\ 等，SHParseDisplayName 只认绝对路径）
        for (auto& p : all) {
            if (p.size() < 2 || (p[1] != L':' && !(p[0] == L'\\' && p[1] == L'\\'))) {
                wchar_t full[MAX_PATH] = {};
                if (GetFullPathNameW(p.c_str(), MAX_PATH, full, nullptr))
                    p = full;
            }
        }
        if (!all.empty()) {
            if (!S().menu.shellMenu) {
                // 自定义菜单：不调 shell 扩展，直接在 UI 线程跑（不会卡）
                ShowCustomContextMenu(g_hwnd, all);
            } else {
                std::vector<std::wstring>* pp = new std::vector<std::wstring>(all);
                HANDLE h = CreateThread(nullptr, 0, ShellMenuThread, pp, 0, nullptr);
                if (h) CloseHandle(h); else delete pp;
            }
        }
    } else if (cmd == L"getSettings") {
        // 把当前设置（含默认值）序列化给页面。页面用 HTML 面板编辑。
        AppSettings& c = S();
        auto B = [](bool b) -> std::wstring { return b ? L"true" : L"false"; };
        std::wstring js = L"{\"kind\":\"settings\",\"data\":{";
        js += L"\"scan\":{\"threads\":" + std::to_wstring(c.threads) +
              L",\"skipHidden\":" + B(c.skipHidden) +
              L",\"followReparse\":" + B(c.followReparse) + L"},";
        js += L"\"treemap\":{\"maxDepth\":" + std::to_wstring(c.maxDepth) +
              L",\"minSizeMB\":" + std::to_wstring(c.minSizeMB) +
              L",\"maxRects\":" + std::to_wstring(c.maxRects) +
              L",\"kidsCap\":" + std::to_wstring(c.kidsCap) +
              L",\"lazyDepth\":" + std::to_wstring(c.lazyDepth) +
              L",\"showLabels\":" + B(c.showLabels) +
              L",\"colorScheme\":" + std::to_wstring(c.colorScheme) + L"},";
        js += L"\"ui\":{\"defaultSort\":" + std::to_wstring(c.defaultSort) +
              L",\"rowExtraPx\":" + std::to_wstring(c.rowExtraPx) +
              L",\"theme\":" + std::to_wstring(c.theme) +
              L",\"confirmDelete\":" + B(c.confirmDelete) + L"},";
        MenuConfig& m = c.menu;
        js += L"\"menu\":{";
        js += L"\"open\":" + B(m.open) + L",\"explore\":" + B(m.explore) +
              L",\"terminal\":" + B(m.terminal) + L",\"props\":" + B(m.props);
        js += L",\"copyPath\":" + B(m.copyPath) + L",\"copyName\":" + B(m.copyName) +
              L",\"copySize\":" + B(m.copySize);
        js += L",\"enter\":" + B(m.enter) + L",\"locateList\":" + B(m.locateList) +
              L",\"locateTreemap\":" + B(m.locateTreemap);
        js += L",\"selectParent\":" + B(m.selectParent) + L",\"goUp\":" + B(m.goUp) +
              L",\"back\":" + B(m.back) + L",\"forward\":" + B(m.forward);
        js += L",\"rescan\":" + B(m.rescan) + L",\"deleteRecycle\":" + B(m.deleteRecycle) +
              L",\"deleteForever\":" + B(m.deleteForever) + L",\"shellMenu\":" + B(m.shellMenu);
        js += L"}}}";   // 关 menu / data / 最外层
        LogLine(L"getSettings json=" + js);
        std::wstring* p = new std::wstring(js);
        if (!PostMessageW(g_hwnd, WM_APP_WEBJSON, 0, (LPARAM)p)) delete p;
    } else if (cmd == L"setSettings") {
        // 页面改设置：整体写回 config.json（settings.cpp 的双向绑定），下次扫描即生效。
        AppSettings c = S();
        auto getBool = [&](const wchar_t* k, bool def) -> bool {
            std::wstring pat = std::wstring(L"\"") + k + L"\":";
            size_t q = json.find(pat);
            if (q == std::wstring::npos) return def;
            q += pat.size();
            while (q < json.size() && (json[q] == L' ')) q++;
            if (json.compare(q, 4, L"true") == 0) return true;
            if (json.compare(q, 5, L"false") == 0) return false;
            return def;
        };
        auto getInt = [&](const wchar_t* k, int def) -> int {
            std::wstring pat = std::wstring(L"\"") + k + L"\":";
            size_t q = json.find(pat);
            if (q == std::wstring::npos) return def;
            q += pat.size();
            while (q < json.size() && (json[q] == L' ')) q++;
            return _wtoi(json.c_str() + q);
        };
        c.threads = getInt(L"threads", c.threads);
        if (c.threads < 1) c.threads = 1;
        if (c.threads > 16) c.threads = 16;
        // 以下几个 0 表示"无限"（对应界面里的 0=∞），不能再夹到下限：
        c.maxDepth = getInt(L"maxDepth", c.maxDepth);       // 0 = 无限层级
        c.maxRects = getInt(L"maxRects", c.maxRects);       // 0 = 不限矩形数
        c.kidsCap  = getInt(L"kidsCap",  c.kidsCap);        // 0 = 每层不限
        c.lazyDepth = getInt(L"lazyDepth", c.lazyDepth);    // 0=全量；层级之下懒加载
        if (c.lazyDepth < 0) c.lazyDepth = 0;
        if (c.lazyDepth > 99) c.lazyDepth = 99;
        c.skipHidden = getBool(L"skipHidden", c.skipHidden);
        c.followReparse = getBool(L"followReparse", c.followReparse);
        c.minSizeMB = getInt(L"minSizeMB", c.minSizeMB);
        c.showLabels = getBool(L"showLabels", c.showLabels);
        c.colorScheme = getInt(L"colorScheme", c.colorScheme);
        c.defaultSort = getInt(L"defaultSort", c.defaultSort);
        c.rowExtraPx = getInt(L"rowExtraPx", c.rowExtraPx);
        c.confirmDelete = getBool(L"confirmDelete", c.confirmDelete);
        MenuConfig& m = c.menu;
        m.open = getBool(L"open", m.open);
        m.explore = getBool(L"explore", m.explore);
        m.terminal = getBool(L"terminal", m.terminal);
        m.props = getBool(L"props", m.props);
        m.copyPath = getBool(L"copyPath", m.copyPath);
        m.copyName = getBool(L"copyName", m.copyName);
        m.copySize = getBool(L"copySize", m.copySize);
        m.enter = getBool(L"enter", m.enter);
        m.locateList = getBool(L"locateList", m.locateList);
        m.locateTreemap = getBool(L"locateTreemap", m.locateTreemap);
        m.selectParent = getBool(L"selectParent", m.selectParent);
        m.goUp = getBool(L"goUp", m.goUp);
        m.back = getBool(L"back", m.back);
        m.forward = getBool(L"forward", m.forward);
        m.rescan = getBool(L"rescan", m.rescan);
        m.deleteRecycle = getBool(L"deleteRecycle", m.deleteRecycle);
        m.deleteForever = getBool(L"deleteForever", m.deleteForever);
        m.shellMenu = getBool(L"shellMenu", m.shellMenu);
        SaveSettings(c);
        g_settings = c;
        g_settingsLoaded = true;
        LogLine(L"settings saved");
    } else if (cmd == L"ready") {
        // push current scan mode (MFT/walk) so the top-bar toggle can show it
        EvalJs(L"window.__mftMode && window.__mftMode(" + std::to_wstring(MftMode()) +
               L"," + (IsElevated() ? L"true" : L"false") + L");");
        LogLine(L"mft push mode=" + std::to_wstring(MftMode()) +
               L" admin=" + std::to_wstring(IsElevated() ? 1 : 0) + L"");
        // 推预字库（页面缓存；扫描二进制的 nameIdx < libCount 时按此索引取词）
        {
            std::wstring js = L"{\"kind\":\"libwords\",\"words\":[";
            for (size_t i = 0; i < g_libWords.size(); i++) {
                if (i) js += L",";
                js += L"\"" + JsEscape(g_libWords[i]) + L"\"";
            }
            js += L"]}";
            auto* p = new std::wstring(js);
            if (!PostMessageW(g_hwnd, WM_APP_WEBJSON, 0, (LPARAM)p)) delete p;
            LogLine(L"lib pushed words=" + std::to_wstring(g_libWords.size()));
        }
        // 把上次的【界面偏好 blob】推给页面 —— 页面自己解析并恢复全部 UI 状态。
        // 主题、面积算法、滑杆值……都在这一个 blob 里，C++ 不关心内容。
        {
            std::wstring blob = LoadPrefsBlob();
            std::wstring esc;
            for (wchar_t c : blob) {
                if (c == L'"' || c == L'\\') { esc += L'\\'; esc += c; }
                else if (c == L'\n') esc += L"\\n";
                else if (c == L'\r') esc += L"\\r";
                else if (c == L'\t') esc += L"\\t";
                else esc += c;
            }
            std::wstring* p = new std::wstring(
                L"{\"kind\":\"prefs\",\"json\":\"" + esc + L"\"}");
            if (!PostMessageW(g_hwnd, WM_APP_WEBJSON, 0, (LPARAM)p)) delete p;
            LogLine(L"pushed prefs bytes=" + std::to_wstring(blob.size()));
        }
        // 把上次的扫描路径推给页面，填进输入框
        std::wstring last = GetSetting(L"lastPath", L"");
        if (!last.empty()) {
            EvalJs(L"window.__setPathFromHost && window.__setPathFromHost(\"" +
                   JsEscape(last) + L"\");");
            LogLine(L"pushed lastPath: " + last);
        }
    } else if (cmd == L"setMft") {
        // mode 是数字（{"mode":1}），getStr 只认带引号字符串，需单独解析
        int m = 0;
        {
            std::wstring pat = L"\"mode\":";
            size_t p = json.find(pat);
            if (p != std::wstring::npos)
                m = _wtoi(json.c_str() + p + pat.size());
        }
        if (m != 0 && m != 1) m = (MftMode() == 0) ? 1 : 0;
        SetSetting(L"mft", std::to_wstring(m));
        LogLine(L"mft mode set to " + std::to_wstring(m));
        // ack back so the button always mirrors real state
        EvalJs(L"window.__mftMode && window.__mftMode(" + std::to_wstring(m) + L"," +
               std::wstring(IsElevated() ? L"true" : L"false") + L");");
        // Switching to MFT while not admin: re-launch elevated (UAC). If the user
        // declines, revert to walk mode and tell the page.
        if (m == 1 && !IsElevated()) {
            // 同步提权：点击切换时弹 UAC。用户确认 → 管理员实例（带 --elevated=1
            // 与 --autoscan=<上次路径>）启动并接管，本实例退出；用户拒绝 → 回退普通遍历。
            wchar_t exe[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            std::wstring last = GetSetting(L"lastPath", L"");
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
                LogLine(L"MFT switch: elevation accepted, exiting walk instance");
                PostMessageW(g_hwnd, WM_CLOSE, 0, 0);   // 关闭本实例，管理员实例接管
            } else {
                SetSetting(L"mft", L"0");
                LogLine(L"elevation declined on MFT switch, reverted to walk");
                EvalJs(L"window.__mftMode && window.__mftMode(0," +
                       std::wstring(IsElevated() ? L"true" : L"false") + L");");
            }
        }
    } else if (cmd == L"open") {
        std::wstring p = getStr(L"path");
        if (!p.empty())
            ShellExecuteW(g_hwnd, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (cmd == L"list") {
        // 页面进入一个 JSON 里没带子项的目录：按需返回它的直接子项
        long long id = _wtoi64(getStr(L"id").c_str());
        auto it = g_idMap.find(id);
        if (it != g_idMap.end() && it->second) {
            ScanNode* n = it->second;
            std::wstring js = L"{\"kind\":\"children\",\"id\":" + std::to_wstring(id) +
                              L",\"children\":[";
            // 按大小降序，最多 3000 个
            std::vector<ScanNode*> kids(n->children.begin(), n->children.end());
            std::stable_sort(kids.begin(), kids.end(),
                             [](const ScanNode* a, const ScanNode* b) {
                                 if (a->isDir != b->isDir) return a->isDir;
                                 return a->size > b->size;
                             });
            if (kids.size() > 3000) kids.resize(3000);
            for (size_t i = 0; i < kids.size(); i++) {
                if (i) js += L",";
                std::wstring one;
                NodeToJson(kids[i], 0, 2, 3000, one, &g_nextId);   // 子项自带 2 层
                js += one;
            }
            js += L"]}";
            auto* payload = new std::wstring(js);
            if (!PostMessageW(g_hwnd, WM_APP_WEBJSON, 0, (LPARAM)payload)) delete payload;
            LogLine(L"list id=" + std::to_wstring(id) + L" kids=" +
                    std::to_wstring(kids.size()));
        } else {
            EvalJs(L"document.getElementById('st0').textContent="
                   L"'该目录数据未保留（请重新扫描）';");
        }
    } else if (cmd == L"browse") {
        BROWSEINFOW bi{};
        bi.hwndOwner = g_hwnd;
        bi.lpszTitle = L"选择要扫描的文件夹";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
        if (pidl) {
            wchar_t path[MAX_PATH]{};
            if (SHGetPathFromIDListW(pidl, path)) {
                EvalJs(std::wstring(L"document.getElementById('path').value=\"") +
                       JsEscape(path) + L"\";");
                SetSetting(L"lastPath", path);
                LogLine(L"saved lastPath(browse)=" + std::wstring(path));
            }
            CoTaskMemFree(pidl);
        }
    } else if (cmd == L"pref") {
        // 通用界面偏好：op = "load" | "save"
        // C++ 不解析内容，只当字符串存取（schema 归 JS）。
        std::wstring op = getStr(L"op");
        if (op == L"save") {
            // json 字段内容可能含任意字符 → 用原始 json 里 "json": 之后的部分
            size_t p = json.find(L"\"json\":\"");
            std::wstring blob;
            if (p != std::wstring::npos) {
                size_t i = p + 8;
                for (; i < json.size(); i++) {
                    wchar_t c = json[i];
                    if (c == L'\\' && i + 1 < json.size()) {
                        wchar_t nx = json[++i];
                        if (nx == L'n') blob += L'\n';
                        else if (nx == L't') blob += L'\t';
                        else if (nx == L'r') blob += L'\r';
                        else blob += nx;
                    } else if (c == L'"') break;
                    else blob += c;
                }
            }
            SavePrefsBlob(blob);
            LogLine(L"pref saved bytes=" + std::to_wstring(blob.size()));
        } else {
            std::wstring blob = LoadPrefsBlob();
            // 回传：把 blob 里的 " \ 转义后塞进 JSON 字符串
            std::wstring esc;
            for (wchar_t c : blob) {
                if (c == L'"' || c == L'\\') { esc += L'\\'; esc += c; }
                else if (c == L'\n') esc += L"\\n";
                else if (c == L'\r') esc += L"\\r";
                else if (c == L'\t') esc += L"\\t";
                else esc += c;
            }
            std::wstring* p = new std::wstring(
                L"{\"kind\":\"prefs\",\"json\":\"" + esc + L"\"}");
            if (!PostMessageW(g_hwnd, WM_APP_WEBJSON, 0, (LPARAM)p)) delete p;
            LogLine(L"pref loaded bytes=" + std::to_wstring(blob.size()));
        }
    } else {
        EvalJs(L"window.__host.__hostEcho(" + json + L");");
    }
}

// ---------- 窗口过程 ----------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_APP_PROGRESS: {
            // 进度：合并到「最新值」，用定时器节流发给页面（避免消息洪水堵死 UI 线程）
            if (g_progTimer == 0)
                g_progTimer = SetTimer(hwnd, 1, 100, nullptr);
            return 0;
        }
        case WM_TIMER: {
            if (wParam == 1) {
                if (InterlockedExchange(&g_progPending, 0)) {
                    wchar_t buf[256];
                    swprintf(buf, 256,
                             L"{\"kind\":\"progress\",\"items\":%llu,\"bytes\":%llu}",
                             g_progItems, g_progBytes);
                    if (g_webview) g_webview->PostWebMessageAsJson(buf);
                    if (GetEnvironmentVariableW(L"DISKMATE_LOG", nullptr, 0))
                        LogLine(std::wstring(L"PROGRESS ") + buf);
                } else {
                    KillTimer(hwnd, 1);
                    g_progTimer = 0;
                }
            }
            return 0;
        }
        case WM_APP_SHELLMENU: {
            // 在消息循环里弹壳菜单（见 cmd=="menu" 处的说明）
            std::wstring* p = (std::wstring*)lParam;
            if (p) {
                ShowShellContextMenu(hwnd, *p);
                delete p;
            }
            return 0;
        }
        case WM_APP_WEBJSON: {
            // 在 UI 线程把 JSON 交给页面（WebView2 只在 UI 线程可用）
            std::wstring* p = (std::wstring*)lParam;
            if (p) {
                if (g_webview) {
                    HRESULT hr = g_webview->PostWebMessageAsJson(p->c_str());
                    if (FAILED(hr)) LogLine(L"UI PostMsg hr=" + std::to_wstring(hr));
                    else if (GetEnvironmentVariableW(L"DISKMATE_SELFTEST", nullptr, 0))
                        g_webview->ExecuteScript(L"setTimeout(()=>window.__runSelfTest(),400);",
                                                 nullptr);
                }
                delete p;
            }
            return 0;
        }
        case WM_SIZE: {
            if (g_controller) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                g_controller->put_Bounds(rc);
            }
            return 0;
        }
        case WM_DPICHANGED: {
            // 窗口被拖到另一块显示器（或系统缩放改了）：重设缩放 + 尺寸
            if (g_controller) {
                UINT dpi = HIWORD(wParam);
                if (!dpi) dpi = 96;
                ICoreWebView2Controller3* c3 = nullptr;
                if (SUCCEEDED(g_controller->QueryInterface(
                        IID_ICoreWebView2Controller3, (void**)&c3)) && c3) {
                    c3->put_RasterizationScale((double)dpi / 96.0);
                    c3->Release();
                }
                RECT* nr = (RECT*)lParam;
                if (nr) {
                    SetWindowPos(hwnd, nullptr, nr->left, nr->top,
                                 nr->right - nr->left, nr->bottom - nr->top,
                                 SWP_NOZORDER | SWP_NOACTIVATE);
                }
                RECT rc;
                GetClientRect(hwnd, &rc);
                g_controller->put_Bounds(rc);
            }
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    // ---- 启动自动提权：默认要求管理员；用户拒绝 UAC 则普通模式继续 ----
    // 同步 runas：提权成功 → 管理员实例（带 --elevated=1）接管，本实例退出；
    // 用户拒绝 → runas 返回失败 → 本实例以普通用户继续运行。
    // 提权是进程级唯一入口，必须在创建任何窗口之前完成。
    {
        LPWSTR cl = GetCommandLineW();
        bool alreadyElev = wcsstr(cl, L"--elevated=1") != nullptr;
        if (!alreadyElev && !IsElevated()) {
            wchar_t exe[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            // 原命令行去掉 exe 名，拼上提权标记，避免新实例再次提权（死循环）
            LPWSTR args = cl;
            if (*args == L'"') { args++; while (*args && *args != L'"') args++; if (*args == L'"') args++; }
            else while (*args && *args != L' ') args++;
            while (*args == L' ') args++;
            std::wstring params = std::wstring(args);
            if (!params.empty()) params += L" ";
            params += L"--elevated=1";
            SHELLEXECUTEINFOW sei{};
            sei.cbSize = sizeof(sei);
            sei.lpVerb = L"runas";
            sei.lpFile = exe;
            sei.lpParameters = params.c_str();
            sei.nShow = SW_SHOWNORMAL;
            LogLine(L"startup elevation: requesting admin (sync runas)...");
            if (ShellExecuteExW(&sei)) {
                // 提权成功：管理员实例接管，本实例使命结束
                LogLine(L"startup elevation accepted, exiting this instance");
                return 0;
            }
            LogLine(L"startup elevation declined, continuing as normal user");
        }
    }

    // Admin startup: make sure MFT mode is on. If the user previously ran
    // non-admin (mode was 0), switch to auto (2) so the button shows MFT.
    if (IsElevated() && MftMode() == 0) {
        SetSetting(L"mft", L"2");
        LogLine(L"admin startup: mft auto-enabled (mode 2)");
    }
    // DPI 感知：必须在创建任何窗口之前声明。
    // 不声明的话，系统缩放 >100% 时 Windows 会按 96 DPI 渲染再整幅拉伸 → 画面发糊。
    // 优先用 Per-Monitor V2（Win10 1703+），退回到 System DPI 感知。
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

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    OleInitialize(nullptr);   // 壳菜单（IContextMenu）需要 OLE

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCE(IDI_DISKMATE));
    wc.hIconSm = LoadIconW(hInst, MAKEINTRESOURCE(IDI_DISKMATE));
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(24, 27, 32));
    wc.lpszClassName = L"DiskMateWebWnd";
    RegisterClassExW(&wc);

    // 窗口初始尺寸按当前 DPI 缩放（否则高 DPI 下窗口显得很小）
    UINT dpi0 = 96;
    {
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        // 窗口还没创建：用 GetDpiForSystem()（不能用 GetDpiForWindow(nullptr)，那样返回的是
        // 主屏在「未感知 DPI」下的 96，会把窗口尺寸算错）
        typedef UINT (WINAPI *GetDpiForSystemFn)(void);
        auto getSysDpi = u32 ? (GetDpiForSystemFn)GetProcAddress(u32, "GetDpiForSystem") : nullptr;
        if (getSysDpi) dpi0 = getSysDpi();
        if (!dpi0) {
            HDC hdc = GetDC(nullptr);
            if (hdc) { dpi0 = (UINT)GetDeviceCaps(hdc, LOGPIXELSX); ReleaseDC(nullptr, hdc); }
        }
        if (!dpi0) dpi0 = 96;
    }
    int winW = MulDiv(1280, dpi0, 96);
    int winH = MulDiv(800, dpi0, 96);
    g_hwnd = CreateWindowExW(0, L"DiskMateWebWnd", L"DiskMate — 磁盘空间分析",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, winW, winH,
                             nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;
    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    InitWebView();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
