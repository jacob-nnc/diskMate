// ============================================================================
// DiskMate ImGui 前端 —— 右键菜单（系统壳菜单 + 自绘兜底）
//   移植 src/webview_host.cpp 的 ShowShellContextMenu(Multi) 与
//   ShowCustomContextMenu：菜单项、顺序、分隔线、单击语义与 HTML 版一致。
//   · shellMenu=true（默认）：IShellFolder + IContextMenu，与资源管理器完全一致，
//     在独立 MTA 线程 + 自己的隐藏 owner 窗口里跑（某些 shell 扩展会在
//     QueryContextMenu 里长时间阻塞，放 UI 线程会把界面冻死）
//   · shellMenu=false：自绘 TrackPopupMenu，标签/顺序对齐宿主实现
//   · 多选：所有项同一父目录时合并到同一个 IContextMenu（资源管理器同款）
// ============================================================================
#include "dm.h"
#include "imgui.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <objbase.h>

using namespace dm;

namespace {

const wchar_t* kMenuHostClass = L"DiskMateImguiShellMenuHost";

// 自绘菜单命令 id
enum {
    CM_OPEN = 1, CM_EXPLORE, CM_TERMINAL, CM_PROPS,
    CM_COPYPATH, CM_COPYNAME, CM_COPYSIZE,
    CM_ENTER, CM_BACK, CM_FORWARD, CM_UP, CM_RESCAN,
    CM_DELRECYCLE, CM_DELFOREVER, CM_LOCATELIST, CM_LOCATETREEMAP,
    CM_SELECTPARENT
};

LRESULT CALLBACK ShellOwnerProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcW(h, m, w, l);
}

// 多选：所有项同一父目录时合并到一个 IContextMenu；返回是否执行了命令
bool ShellMenuOnOwner(HWND owner, const std::vector<std::wstring>& paths) {
    if (paths.empty()) return false;
    std::vector<PIDLIST_ABSOLUTE> pidls;
    for (const auto& p : paths) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        SFGAOF a = 0;
        if (SUCCEEDED(SHParseDisplayName(p.c_str(), nullptr, &pidl, 0, &a)) && pidl)
            pidls.push_back(pidl);
    }
    if (pidls.empty()) return false;

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
        for (auto* x : pidls) CoTaskMemFree(x);
        return false;
    }
    std::vector<PCUITEMID_CHILD> children;
    children.push_back(child0);
    for (size_t i = 1; i < pidls.size(); i++) {
        if (parentKey(pidls[i]) != key0) continue;
        IShellFolder* par2 = nullptr;
        PCUITEMID_CHILD c2 = nullptr;
        if (SUCCEEDED(SHBindToParent(pidls[i], IID_IShellFolder, (void**)&par2, &c2)) && par2) {
            children.push_back(c2);
            par2->Release();
        }
    }

    bool invoked = false;
    IContextMenu* cm = nullptr;
    HRESULT hr = parent->GetUIObjectOf(owner, (UINT)children.size(), children.data(),
                                       IID_IContextMenu, nullptr, (void**)&cm);
    if (SUCCEEDED(hr) && cm) {
        HMENU menu = CreatePopupMenu();
        if (menu) {
            const UINT ID_FIRST = 0x8000, ID_LAST = 0x8FFF;
            if (SUCCEEDED(cm->QueryContextMenu(menu, 0, ID_FIRST, ID_LAST, CMF_NORMAL))) {
                POINT pt;
                GetCursorPos(&pt);
                MSG msg;
                PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
                SetForegroundWindow(owner);
                UINT sel = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN,
                                          pt.x, pt.y, 0, owner, nullptr);
                if (sel) {
                    CMINVOKECOMMANDINFOEX info{};
                    info.cbSize = sizeof(info);
                    info.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
                    info.hwnd = owner;
                    info.lpVerbW = MAKEINTRESOURCEW(sel - ID_FIRST);
                    info.lpVerb = MAKEINTRESOURCEA(sel - ID_FIRST);
                    info.nShow = SW_SHOWNORMAL;
                    info.ptInvoke = pt;
                    cm->InvokeCommand((LPCMINVOKECOMMANDINFO)&info);
                    invoked = true;
                }
            }
            DestroyMenu(menu);
        }
        cm->Release();
    }
    parent->Release();
    for (auto* x : pidls) CoTaskMemFree(x);
    LogLine(L"shellmenu paths=" + std::to_wstring(paths.size()) + L" invoked=" +
            (invoked ? L"1" : L"0"));
    return invoked;
}

DWORD WINAPI ShellMenuThread(LPVOID param) {
    auto* pp = (std::vector<std::wstring>*)param;
    std::vector<std::wstring> paths = *pp;
    delete pp;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ShellOwnerProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kMenuHostClass;
    RegisterClassExW(&wc);
    HWND owner = CreateWindowExW(0, kMenuHostClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ShellMenuOnOwner(owner ? owner : GetActiveWindow(), paths);
    CoUninitialize();
    if (owner) DestroyWindow(owner);
    return 0;
}

}  // namespace

void ShowShellContextMenuAsync(std::vector<std::wstring> paths) {
    if (paths.empty()) return;
    auto* pp = new std::vector<std::wstring>(std::move(paths));
    HANDLE h = CreateThread(nullptr, 0, ShellMenuThread, pp, 0, nullptr);
    if (h) CloseHandle(h);
    else delete pp;
}

// 自绘兜底菜单（shellMenu=false 时）：标签 / 顺序 / 分隔线对齐宿主实现
void ShowCustomContextMenu(const std::vector<std::wstring>& paths) {
    if (paths.empty()) return;
    MenuConfig& m = g_st.menu;
    const bool multi = paths.size() > 1;
    const std::wstring& path = paths[0];
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    auto add = [&](bool on, UINT id, const wchar_t* text) {
        if (on) AppendMenuW(menu, MF_STRING, id, text);
    };
    AppendMenuW(menu, MF_STRING, CM_OPEN, multi ? L"打开（多项）" : L"打开");
    add(m.explore, CM_EXPLORE, L"在资源管理器中显示");
    add(m.terminal, CM_TERMINAL, L"在此处打开终端");
    add(m.props, CM_PROPS, L"属性");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(m.copyPath, CM_COPYPATH, L"复制完整路径");
    add(m.copyName, CM_COPYNAME, L"复制文件名");
    add(m.copySize, CM_COPYSIZE, L"复制大小");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(m.enter, CM_ENTER, L"进入此目录");
    add(m.selectParent, CM_SELECTPARENT, L"选择父级");
    add(m.locateList, CM_LOCATELIST, L"在列表中定位");
    add(m.locateTreemap, CM_LOCATETREEMAP, L"在树图中定位");
    add(m.back, CM_BACK, L"后退");
    add(m.forward, CM_FORWARD, L"前进");
    add(m.goUp, CM_UP, L"向上");
    add(m.rescan, CM_RESCAN, L"重新扫描此项");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(m.deleteRecycle, CM_DELRECYCLE, L"删除到回收站");
    add(m.deleteForever, CM_DELFOREVER, L"永久删除");

    POINT pt;
    GetCursorPos(&pt);
    HWND owner = GetActiveWindow();
    SetForegroundWindow(owner);
    UINT sel = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN, pt.x, pt.y, 0,
                              owner, nullptr);
    DestroyMenu(menu);
    if (!sel) return;

    auto node = [&](const std::wstring& p) { return FindNodeByPath(p); };
    switch (sel) {
        case CM_OPEN:
        case CM_ENTER: {
            ScanNode* n = node(path);
            if (n && n->isDir && !multi) Enter(n);
            else if (n && n->isDir && multi) Enter(n);
            else
                for (const auto& p : paths) ShellOpenPath(p);
            break;
        }
        case CM_EXPLORE: ShellExplorePath(path); break;
        case CM_TERMINAL: ShellTerminalPath(path); break;
        case CM_PROPS: ShellPropertiesPath(path); break;
        case CM_COPYPATH: {
            std::wstring all;
            for (const auto& p : paths) { if (!all.empty()) all += L"\r\n"; all += p; }
            ClipboardSetText(all);
            break;
        }
        case CM_COPYNAME: {
            size_t bs = path.find_last_of(L"\\/");
            ClipboardSetText(bs == std::wstring::npos ? path : path.substr(bs + 1));
            break;
        }
        case CM_COPYSIZE: {
            ScanNode* n = node(path);
            ClipboardSetText(n ? FmtSizeW(n->size) : L"");
            break;
        }
        case CM_SELECTPARENT: {
            ScanNode* n = node(path);
            if (n && n->parent) {
                g_selSet.clear();
                g_selSet.insert(n->parent);
                g_selNode = n->parent;
                g_anchorRow = -1;
            }
            break;
        }
        case CM_LOCATELIST: {
            ScanNode* n = node(path);
            if (n) LocateInList(n);
            break;
        }
        case CM_LOCATETREEMAP: {
            ScanNode* n = node(path);
            if (n) { g_selNode = n; TreemapInvalidate(); }
            break;
        }
        case CM_BACK: GoBack(); break;
        case CM_FORWARD: break;   // HTML 也没有前进栈
        case CM_UP: GoUp(); break;
        case CM_RESCAN: StartScan(path); break;
        case CM_DELRECYCLE:
            for (const auto& p : paths) ShellDeletePath(p, false, g_st.confirmDelete);
            break;
        case CM_DELFOREVER:
            for (const auto& p : paths) ShellDeletePath(p, true, g_st.confirmDelete);
            break;
    }
}
