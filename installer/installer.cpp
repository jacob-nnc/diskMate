#include <windows.h>
#include <shlobj.h>
#include <objbase.h>
#include <string>

#define IDI_APP 101
#define IDR_PAYLOAD_1 201
#define IDR_PAYLOAD_2 202
#define IDR_PAYLOAD_3 203
#define IDR_PAYLOAD_4 204
#define IDR_PAYLOAD_5 205
#define IDR_PAYLOAD_6 206
#define IDR_PAYLOAD_7 207

#define IDC_EDIT_DIR   1001
#define IDC_BTN_BROWSE 1002
#define IDC_BTN_INSTALL 1003
#define IDC_BTN_EXIT   1004
#define IDC_ST_STATUS  1005

struct PayloadFile {
    int resId;
    const wchar_t* relPath;
};

static const PayloadFile kPayload[] = {
    { IDR_PAYLOAD_1, L"diskmate_web.exe" },
    { IDR_PAYLOAD_2, L"WebView2Loader.dll" },
    { IDR_PAYLOAD_3, L"web\\index.html" },
    { IDR_PAYLOAD_4, L"web\\diskmate_tree.ico" },
    { IDR_PAYLOAD_5, L"diskmate.ini" },
    { IDR_PAYLOAD_6, L"使用说明.txt" },
    { IDR_PAYLOAD_7, L"config.json" },
};
static const int kPayloadCount = 7;
static HINSTANCE g_hInst;

static void SetStatus(HWND hwnd, const wchar_t* s) {
    SetWindowTextW(GetDlgItem(hwnd, IDC_ST_STATUS), s);
}

static bool ExtractResource(HMODULE mod, int resId, const std::wstring& outPath) {
    HRSRC hr = FindResourceW(mod, MAKEINTRESOURCEW(resId), MAKEINTRESOURCEW(10));
    if (!hr) return false;
    HGLOBAL hg = LoadResource(mod, hr);
    if (!hg) return false;
    void* data = LockResource(hg);
    DWORD size = SizeofResource(mod, hr);
    if (!data || size == 0) return false;
    HANDLE f = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(f, data, size, &written, nullptr);
    CloseHandle(f);
    return ok && written == size;
}

static bool MakeDirs(const std::wstring& path) {
    std::wstring cur;
    for (wchar_t ch : path) {
        cur.push_back(ch);
        if (ch == L'\\' || ch == L'/') {
            if (cur.size() > 3) CreateDirectoryW(cur.c_str(), nullptr);
        }
    }
    CreateDirectoryW(path.c_str(), nullptr);
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static bool CreateDesktopShortcut(const std::wstring& target, const std::wstring& linkPath) {
    CoInitialize(nullptr);
    IShellLinkW* sl = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IShellLinkW, (void**)&sl);
    if (FAILED(hr) || !sl) {
        if (sl) sl->Release();
        CoUninitialize();
        return false;
    }
    sl->SetPath(target.c_str());
    sl->SetDescription(L"DiskMate - 磁盘空间分析");
    sl->SetIconLocation(target.c_str(), 0);
    IPersistFile* pf = nullptr;
    hr = sl->QueryInterface(IID_IPersistFile, (void**)&pf);
    bool ok = false;
    if (SUCCEEDED(hr) && pf) {
        ok = SUCCEEDED(pf->Save(linkPath.c_str(), TRUE));
        pf->Release();
    }
    sl->Release();
    CoUninitialize();
    return ok;
}

static std::wstring GetShellPath(int csidl) {
    wchar_t buf[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT, buf);
    return buf;
}

static void DoInstall(HWND hwnd) {
    wchar_t dir[1024];
    GetWindowTextW(GetDlgItem(hwnd, IDC_EDIT_DIR), dir, 1024);
    std::wstring root = dir;
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
    if (root.empty()) {
        MessageBoxW(hwnd, L"请先选择安装目录", L"DiskMate", MB_ICONWARNING);
        return;
    }
    if (!MakeDirs(root)) {
        MessageBoxW(hwnd, L"无法创建安装目录", L"DiskMate", MB_ICONERROR);
        return;
    }
    for (int i = 0; i < kPayloadCount; i++) {
        std::wstring full = root + L"\\" + kPayload[i].relPath;
        size_t bs = full.find_last_of(L"\\/");
        if (bs != std::wstring::npos) MakeDirs(full.substr(0, bs));
        SetStatus(hwnd, (std::wstring(L"正在安装: ") + kPayload[i].relPath).c_str());
        if (!ExtractResource(g_hInst, kPayload[i].resId, full)) {
            MessageBoxW(hwnd, (std::wstring(L"文件释放失败: ") + kPayload[i].relPath).c_str(),
                        L"DiskMate", MB_ICONERROR);
            return;
        }
    }
    std::wstring exePath = root + L"\\diskmate_web.exe";
    std::wstring linkPath = GetShellPath(CSIDL_DESKTOPDIRECTORY) + L"\\DiskMate 磁盘分析.lnk";
    CreateDesktopShortcut(exePath, linkPath);
    SetStatus(hwnd, L"安装完成");
    MessageBoxW(hwnd,
        L"安装完成！已在桌面创建快捷方式。\n\n"
        L"提示：程序启动时自动请求管理员权限，\n"
        L"同意后即可使用 MFT 直读高速扫描。",
        L"DiskMate", MB_ICONINFORMATION);
}

static void BrowseDir(HWND hwnd) {
    BROWSEINFOW bi{};
    bi.hwndOwner = hwnd;
    bi.lpszTitle = L"选择安装目录";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH] = {};
        if (SHGetPathFromIDListW(pidl, path))
            SetWindowTextW(GetDlgItem(hwnd, IDC_EDIT_DIR), path);
        CoTaskMemFree(pidl);
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HWND hTitle = CreateWindowW(L"STATIC", L"DiskMate 安装程序", WS_CHILD | WS_VISIBLE,
                                    20, 16, 300, 26, hwnd, nullptr, g_hInst, nullptr);
        HFONT hfTitle = CreateFontW(19, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                                    0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
        SendMessageW(hTitle, WM_SETFONT, (WPARAM)hfTitle, TRUE);
        CreateWindowW(L"STATIC", L"安装目录：", WS_CHILD | WS_VISIBLE,
                      20, 62, 100, 20, hwnd, nullptr, g_hInst, nullptr);
        std::wstring def = GetShellPath(CSIDL_LOCAL_APPDATA) + L"\\Programs\\DiskMate";
        CreateWindowW(L"EDIT", def.c_str(), WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                      20, 86, 296, 24, hwnd, (HMENU)IDC_EDIT_DIR, g_hInst, nullptr);
        CreateWindowW(L"BUTTON", L"浏览…", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                      324, 86, 76, 24, hwnd, (HMENU)IDC_BTN_BROWSE, g_hInst, nullptr);
        CreateWindowW(L"BUTTON", L"安装", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                      190, 148, 90, 30, hwnd, (HMENU)IDC_BTN_INSTALL, g_hInst, nullptr);
        CreateWindowW(L"BUTTON", L"退出", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                      292, 148, 90, 30, hwnd, (HMENU)IDC_BTN_EXIT, g_hInst, nullptr);
        CreateWindowW(L"STATIC", L"就绪", WS_CHILD | WS_VISIBLE,
                      20, 196, 380, 20, hwnd, (HMENU)IDC_ST_STATUS, g_hInst, nullptr);
        SendMessageW(hwnd, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDC_BTN_INSTALL) DoInstall(hwnd);
        else if (id == IDC_BTN_BROWSE) BrowseDir(hwnd);
        else if (id == IDC_BTN_EXIT) DestroyWindow(hwnd);
        break;
    }
    case WM_CLOSE: DestroyWindow(hwnd); break;
    case WM_DESTROY: PostQuitMessage(0); break;
    default: return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    g_hInst = hInst;
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP));
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"DiskMateSetupWnd";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(L"DiskMateSetupWnd", L"DiskMate 安装程序",
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                              CW_USEDEFAULT, CW_USEDEFAULT, 436, 276,
                              nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
