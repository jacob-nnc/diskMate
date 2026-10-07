// ============================================================================
// FileList —— 左侧文件列表管理器实现
//   行数据 / 展开 / 选中 / 悬浮父行 / 滚动 / 历史 / 自绘 / 命中 全部内聚。
//   箭头与名称在同一「名称行」内垂直居中（修复箭头/文字垂直错位）。
//   行窗口行高 = ListView 行高（滚动单位一致，展开项不会跳到看不见的位置）。
// ============================================================================
#include "filelist.h"
#include "theme.h"
#include <windowsx.h>
#include <algorithm>
#include <shellapi.h>
#include <shlobj.h>

extern double g_dpiScale;   // main.cpp
extern HFONT g_uiFont;      // main.cpp
extern HFONT g_glyphFont;   // main.cpp（▸/▾ 字形专用）

// 日志统一走 main.cpp 的 LogLine（同一 diskmate.log，避免双流 ccs 打开冲突）
extern void LogLine(const std::wstring& s);
static void LogFl(const std::wstring& s) { LogLine(s); }

static int S(int v) { return (int)(v * g_dpiScale + 0.5); }

namespace {
// 主题指标 / 取色（theme.h：C()=colors M()=metrics）
int TMetric(int kind) {
    const ThemeMetrics& m = ThemeStore::Instance().M();
    switch (kind) {
        case 0: return m.padX;
        case 1: return m.indent;
        default: return m.arrowBox;
    }
}
COLORREF TC(int idx) {
    const ThemeColors& c = ThemeStore::Instance().C();
    switch (idx) {
        case 0: return c.text;
        case 1: return c.bg;
        case 2: return c.selection;
        case 3: return c.textDim;
        case 4: return c.treeDir;
        case 5: return c.arrow;
        case 6: return c.panel;
        default: return c.line;
    }
}
}  // namespace

FileList::~FileList() {
    if (rowImg_) ImageList_Destroy(rowImg_);
    for (HWND h : rowWnds_)
        if (h) DestroyWindow(h);
    if (host_) DestroyWindow(host_);
    if (upRow_) DestroyWindow(upRow_);
    if (list_) DestroyWindow(list_);
}

std::wstring FileList::FormatSize(ULONGLONG b) {
    wchar_t buf[64];
    if (b >= (1ULL << 40)) swprintf(buf, 64, L"%.2f TB", (double)b / (1ULL << 40));
    else if (b >= (1ULL << 30)) swprintf(buf, 64, L"%.2f GB", (double)b / (1ULL << 30));
    else if (b >= (1ULL << 20)) swprintf(buf, 64, L"%.2f MB", (double)b / (1ULL << 20));
    else if (b >= (1ULL << 10)) swprintf(buf, 64, L"%.1f KB", (double)b / (1ULL << 10));
    else swprintf(buf, 64, L"%llu B", b);
    return buf;
}

std::wstring FileList::FormatCount(ULONGLONG n) {
    wchar_t buf[48];
    if (n >= 100000000ULL) swprintf(buf, 48, L"%.1f 亿", (double)n / 100000000.0);
    else if (n >= 10000) swprintf(buf, 48, L"%.1f 万", (double)n / 10000.0);
    else swprintf(buf, 48, L"%llu", n);
    return buf;
}

static std::wstring TypeOfNode(const ScanNode* n) {
    if (n->isDir) return L"文件夹";
    std::wstring name = n->name;
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == name.size() - 1) return L"文件";
    return name.substr(dot);
}

// ---------------------------------------------------------------- 创建 ----
void FileList::Create(HWND parent, HINSTANCE inst, const AppSettings* settings) {
    st_ = settings;
    inst_ = inst;
    parent_ = parent;

    // 注册行窗口 / 「..」行窗口 / 行容器类（首次）
    static bool s_reg = false;
    if (!s_reg) {
        WNDCLASSEXW rc{};
        rc.cbSize = sizeof(rc);
        rc.hInstance = inst;
        rc.lpfnWndProc = RowProc;
        rc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        rc.hbrBackground = nullptr;
        rc.lpszClassName = L"DiskMateFLRow";
        RegisterClassExW(&rc);
        rc.lpfnWndProc = UpRowProc;
        rc.lpszClassName = L"DiskMateFLUpRow";
        RegisterClassExW(&rc);
        rc.lpfnWndProc = DefWindowProcW;
        rc.hbrBackground = (HBRUSH)GetStockObject(DC_BRUSH);
        rc.lpszClassName = L"DiskMateFLRowHost";
        RegisterClassExW(&rc);
        s_reg = true;
        LogFl(L"[FL] classes registered");
    }

    // 行容器（盖住列表客户区，承载每行自绘窗口）
    host_ = CreateWindowExW(0, L"DiskMateFLRowHost", L"",
                            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                            0, 0, 0, 0, parent, nullptr, inst, nullptr);
    // ListView（滚动容器）
    list_ = CreateWindowExW(0, WC_LISTVIEWW, L"",
                            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL |
                                LVS_SHOWSELALWAYS | LVS_OWNERDATA | LVS_EX_DOUBLEBUFFER,
                            0, 0, 0, 0, parent, nullptr, inst, nullptr);
    SendMessageW(list_, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
    header_ = ListView_GetHeader(list_);
    // 五列：名称吃剩余宽度
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = (LPWSTR)L"名称";
    col.cx = 300;
    ListView_InsertColumn(list_, 0, &col);
    col.pszText = (LPWSTR)L"大小";
    col.cx = S(90);
    ListView_InsertColumn(list_, 1, &col);
    col.pszText = (LPWSTR)L"占比";
    col.cx = S(70);
    ListView_InsertColumn(list_, 2, &col);
    col.pszText = (LPWSTR)L"类型";
    col.cx = S(70);
    ListView_InsertColumn(list_, 3, &col);
    col.pszText = (LPWSTR)L"文件数";
    col.cx = S(70);
    ListView_InsertColumn(list_, 4, &col);
    SendMessageW(list_, LVM_SETEXTENDEDLISTVIEWSTYLE,
                 LVS_EX_DOUBLEBUFFER, LVS_EX_DOUBLEBUFFER);

    // 固定「..」行（不随列表滚动）
    upRow_ = CreateWindowExW(0, L"DiskMateFLUpRow", L"",
                             WS_CHILD | WS_VISIBLE,
                             0, 0, 0, 0, parent, nullptr, inst_, (LPVOID)this);
    ShowWindow(upRow_, SW_HIDE);

    padX_ = TMetric(0);
    indent_ = TMetric(1);
    arrowBox_ = TMetric(2);

    // ListView 子类化：拦截左键（行窗口层已接管行点击，防止 ListView 抢选中）
    SetWindowSubclass(list_, ListProc, 0, (DWORD_PTR)this);
    wchar_t fb[160];
    swprintf(fb, 160, L"[FL] create host=%p list=%p up=%p header=%p",
            (void*)host_, (void*)list_, (void*)upRow_, (void*)header_);
    LogFl(fb);
}

// ---------------------------------------------------------------- 排序 ----
void FileList::SetSort(int col, bool asc) {
    sortCol_ = col;
    sortAsc_ = asc;
    SortView();
    Rebuild();
    Refresh();
}

void FileList::SortView() {
    auto cmp = [&](const ScanNode* a, const ScanNode* b) {
        int r = 0;
        switch (sortCol_) {
            case 0: r = _wcsicmp(a->name.c_str(), b->name.c_str()); break;
            case 1: case 2: r = a->size < b->size ? -1 : (a->size > b->size ? 1 : 0); break;
            case 3: {
                r = _wcsicmp(TypeOfNode(a).c_str(), TypeOfNode(b).c_str());
                if (r == 0) r = _wcsicmp(a->name.c_str(), b->name.c_str());
                break;
            }
            case 4: r = a->fileCount < b->fileCount ? -1 : (a->fileCount > b->fileCount ? 1 : 0); break;
        }
        return sortAsc_ ? r < 0 : r > 0;
    };
    std::stable_sort(view_.begin(), view_.end(), cmp);
}

bool FileList::CanExpand(const ScanNode* n) const {
    if (!n || !n->isDir || n->children.empty()) return false;
    for (auto* c : n->children)
        if (c) return true;
    return false;
}

std::vector<ScanNode*> FileList::SortedChildren(ScanNode* dir) {
    std::vector<ScanNode*> kids;
    for (auto* c : dir->children)
        if (c) kids.push_back(c);
    auto cmp = [&](const ScanNode* a, const ScanNode* b) {
        int r = 0;
        switch (sortCol_) {
            case 0: r = _wcsicmp(a->name.c_str(), b->name.c_str()); break;
            case 1: case 2: r = a->size < b->size ? -1 : (a->size > b->size ? 1 : 0); break;
            case 3: {
                r = _wcsicmp(TypeOfNode(a).c_str(), TypeOfNode(b).c_str());
                if (r == 0) r = _wcsicmp(a->name.c_str(), b->name.c_str());
                break;
            }
            case 4: r = a->fileCount < b->fileCount ? -1 : (a->fileCount > b->fileCount ? 1 : 0); break;
        }
        return sortAsc_ ? r < 0 : r > 0;
    };
    std::stable_sort(kids.begin(), kids.end(), cmp);
    return kids;
}

// ------------------------------------------------------------ 行数据 ----
void FileList::SetCurrent(ScanNode* node, const std::vector<ScanNode*>& view) {
    current_ = node;
    view_ = view;
    // 子项位置变化后旧的展开态失效：只保留当前视图内仍可见的展开目录
    auto inView = [&](ScanNode* n) {
        if (n == current_) return true;
        for (auto* v : view_) if (v == n) return true;
        return false;
    };
    for (auto it = expanded_.begin(); it != expanded_.end();) {
        if (!inView(*it)) it = expanded_.erase(it);
        else ++it;
    }
    SortView();
    Rebuild();
    Refresh();
}

void FileList::NavigateTo(ScanNode* node, bool pushHistory) {
    if (!node) return;
    if (pushHistory && current_ && current_ != node) back_.push_back(current_);
    if (pushHistory) fwd_.clear();
    SetCurrent(node, node->children);
}

void FileList::GoUp() {
    if (current_ && current_->parent) NavigateTo(current_->parent, true);
}

void FileList::GoBack() {
    if (back_.empty()) return;
    fwd_.push_back(current_);
    ScanNode* target = back_.back();
    back_.pop_back();
    SetCurrent(target, target->children);
}

void FileList::GoForward() {
    if (fwd_.empty()) return;
    back_.push_back(current_);
    ScanNode* target = fwd_.back();
    fwd_.pop_back();
    SetCurrent(target, target->children);
}

void FileList::ResetScan() {
    expanded_.clear();
    back_.clear();
    fwd_.clear();
    rows_.clear();
    rowView_.clear();
    current_ = nullptr;
    selRow_ = -1;
    scrollY_ = 0;
    pinnedRow_ = -1;
    pinNode_ = nullptr;
    pinLabel_.clear();
    if (list_) ListView_SetItemCountEx(list_, 0, LVSICF_NOSCROLL);
}

void FileList::Rebuild() {
    rows_.clear();
    rowView_.clear();
    selRow_ = -1;  // 行结构变化：选中失效
    if (CanGoUp()) {
        Row h;
        h.isParent = true;
        h.hasChildren = CanExpand(current_);
        h.expanded = expanded_.count(current_) > 0;
        h.depth = 0;
        h.node = current_;
        rows_.push_back(h);
        rowView_.push_back(-1);
    }
    // 顶层行直接递归展开（子项不在 view_ 里，不能用 ViewIndexOf —— 那是
    // 「展开后子项永不显示」的历史根因）
    std::function<void(ScanNode*, int)> walk = [&](ScanNode* n, int depth) {
        bool has = CanExpand(n);
        bool exp = has && expanded_.count(n) > 0;
        Row r;
        r.hasChildren = has;
        r.expanded = exp;
        r.depth = depth;
        r.node = n;
        rows_.push_back(r);
        rowView_.push_back(-1);
        if (exp) {
            auto kids = SortedChildren(n);
            for (auto* k : kids) walk(k, depth + 1);
        }
    };
    for (size_t i = 0; i < view_.size(); i++) {
        size_t mark = rows_.size();
        walk(view_[i], 0);
        for (size_t k = mark; k < rows_.size(); k++) rowView_[k] = (int)i;
    }
}

const FileList::Row& FileList::RowAt(int idx) const {
    static Row s_empty;
    if (idx >= 0 && idx < (int)rows_.size()) return rows_[idx];
    return s_empty;
}

FileList::Row FileList::EffectiveRow(int idx) const {
    Row r = (idx >= 0 && idx < (int)rows_.size()) ? rows_[idx] : Row{};
    if (idx == 0 && pinnedRow_ == 0 && !r.isParent && pinNode_) {
        r.node = pinNode_;
        r.depth = 0;
        r.hasChildren = CanExpand(pinNode_);
        r.expanded = expanded_.count(pinNode_) > 0;
    }
    return r;
}

ScanNode* FileList::SelNode() const {
    if (selRow_ >= 0 && selRow_ < (int)rows_.size()) return rows_[selRow_].node;
    return nullptr;
}

// ------------------------------------------------------------ 布局 ----
int FileList::RowHeightPx() const {
    int h = S(22);
    if (st_ && st_->rowExtraPx > 0) h += S(st_->rowExtraPx);
    return h < S(18) ? S(18) : h;
}

void FileList::EnsureRowWnds(int need) {
    while ((int)rowWnds_.size() < need) {
        int idx = (int)rowWnds_.size();
        HWND h = CreateWindowExW(0, L"DiskMateFLRow", L"", WS_CHILD | WS_VISIBLE,
                                 0, 0, 0, 0, host_, (HMENU)(INT_PTR)(20000 + idx), inst_,
                                 (LPVOID)this);
        rowWnds_.push_back(h);
    }
    wchar_t rw[128];
    swprintf(rw, 128, L"[FL] rowwnds pool=%zu need=%d", rowWnds_.size(), need);
    LogFl(rw);
}

void FileList::Layout(int x, int y, int w, int h, int upH) {
    if (!host_ || !list_) return;
    LogFl(L"[FL] layout x=" + std::to_wstring(x) + L" y=" + std::to_wstring(y) +
          L" w=" + std::to_wstring(w) + L" h=" + std::to_wstring(h) + L" upH=" + std::to_wstring(upH));
    // 列表 + 行容器（避开右侧滚动条）+ 「..」行 的位置统一由这里摆放
    int sbW = GetSystemMetrics(SM_CXVSCROLL);
    MoveWindow(list_, x, y, w, h, TRUE);
    MoveWindow(host_, x, y, w - sbW, h, TRUE);
    MoveWindow(upRow_, x, y - upH, w, upH, TRUE);
    // 行容器必须盖在 ListView 之上（Z 序）：host_ 创建早于 list_ 会被盖住，
    // 表现即“列表空白，鼠标划过才显示 ListView 默认文本”。
    SetWindowPos(host_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(upRow_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(upRow_, CanGoUp() ? SW_SHOW : SW_HIDE);
    InvalidateRect(upRow_, nullptr, TRUE);
    // ListView 行高 = 行窗口行高（滚动单位一致，否则展开项跳位）
    rowH_ = RowHeightPx();
    if (list_) {
        int cur = 0;
        HIMAGELIST now = ListView_GetImageList(list_, LVSIL_SMALL);
        if (now) ImageList_GetIconSize(now, nullptr, &cur);
        if (cur != rowH_) {
            HIMAGELIST hil = ImageList_Create(1, rowH_, ILC_COLOR32, 0, 0);
            if (hil) {
                HIMAGELIST old = ListView_SetImageList(list_, hil, LVSIL_SMALL);
                if (rowImg_ && rowImg_ != old) ImageList_Destroy(rowImg_);
                rowImg_ = hil;
            }
        }
        // 名称列吃剩余宽度
        int fixed = S(90) + S(70) + S(70) + S(70);
        int nameW = w - fixed - S(8) - S(16);
        if (nameW < S(60)) nameW = S(60);
        ListView_SetColumnWidth(list_, 0, nameW);
    }
    PlaceRows();
    UpdatePin();
}

void FileList::Refresh() {
    if (list_) {
        ListView_SetItemCountEx(list_, Count() > 0 ? Count() : 0, 0);
        ListView_RedrawItems(list_, 0, std::max(0, Count() - 1));
    }
    // 导航/展开后行数变化：行窗口必须立刻重建摆放（否则初始列表空白）
    PlaceRows();
    UpdatePin();
}

void FileList::OnListScroll(int topIndex) {
    scrollY_ = topIndex * rowH_;
    PlaceRows();
    UpdatePin();
}

// 按滚动偏移把行窗口摆到可视区（只放可视区，其余隐藏；行窗口池按需增长）
void FileList::PlaceRows() {
    if (!host_) return;
    // Z 序保险：行容器必须盖在 ListView 之上（每次摆放都拉回，防止被重绘/重建压下去）
    SetWindowPos(host_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    RECT hr{};
    GetClientRect(host_, &hr);
    int visible = (rowH_ > 0) ? (hr.bottom / rowH_ + 2) : 0;
    if (visible < 1) visible = 1;
    int need = (int)rows_.size();
    EnsureRowWnds(need);
    int first = (rowH_ > 0) ? (scrollY_ / rowH_) : 0;
    int last = first + visible + 1;
    static ULONGLONG s_lastFlLog = 0;
    ULONGLONG flNow = GetTickCount64();
    if (flNow - s_lastFlLog >= 800) {
        s_lastFlLog = flNow;
        wchar_t lb[160];
        swprintf(lb, 160, L"[FL] place rows=%zu visible=%d first=%d last=%d valid=%d",
                rows_.size(), visible, first, last,
                (int)std::count_if(rowWnds_.begin(), rowWnds_.end(), [](HWND h){ return h != nullptr; }));
        LogFl(lb);
    }
    for (int i = 0; i < (int)rowWnds_.size(); i++) {
        HWND rw = rowWnds_[i];
        if (!rw) continue;
        if (i >= first && i <= last && i < need) {
            int ry = i * rowH_ - scrollY_;
            SetWindowPos(rw, HWND_TOP, 0, ry, hr.right, rowH_, SWP_SHOWWINDOW);
            InvalidateRect(rw, nullptr, TRUE);
        } else {
            ShowWindow(rw, SW_HIDE);
        }
    }
}
// ------------------------------------------------------------ 悬浮 ----
void FileList::UpdatePin() {
    int prevPin = pinnedRow_;
    int newPin = -1;
    std::wstring label;
    int first = -1;
    if (rowH_ > 0 && !rows_.empty() && !rows_[0].isParent) {
        first = scrollY_ / rowH_;
        if (first > 0 && first - 1 < (int)rows_.size()) {
            const Row& pr = rows_[first - 1];
            if (!pr.isParent && pr.hasChildren && pr.expanded && pr.node) {
                newPin = 0;
                label = std::wstring(L"➤ ") + pr.node->name;
            }
        }
    }
    pinnedRow_ = newPin;
    pinLabel_ = label;
    pinNode_ = (newPin == 0 && first - 1 >= 0 && first - 1 < (int)rows_.size())
                   ? rows_[first - 1].node : nullptr;
    if (prevPin != newPin && list_) {
        ListView_RedrawItems(list_, 0, 1);
        InvalidateRect(list_, nullptr, TRUE);
        if (host_) {
            RECT hr{};
            GetClientRect(host_, &hr);
            if (rowH_ > 0 && !rowWnds_.empty() && rowWnds_[0]) {
                int ry = 0 - scrollY_;
                SetWindowPos(rowWnds_[0], HWND_TOP, 0, ry, hr.right, rowH_, SWP_SHOWWINDOW);
                InvalidateRect(rowWnds_[0], nullptr, TRUE);
            }
        }
    }
}

// ------------------------------------------------------------ 绘制 ----
void FileList::DrawRow(HDC dc, RECT rc, int rowIdx, bool selected) {
    Row r = EffectiveRow(rowIdx);
    HBRUSH bg = CreateSolidBrush(selected ? TC(2) : TC(1));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    HFONT old = (HFONT)SelectObject(dc, g_uiFont);
    SetBkMode(dc, TRANSPARENT);

    // 名称行底（「..」行占整行；普通行 = 顶部 20px 名称行）
    int nameBottom = r.isParent ? rc.bottom : rc.top + S(20);
    // 箭头：与名称同在一个名称行内垂直居中（历史错位 = 箭头整行居中、名称顶行居中）
    if (!r.isParent && r.hasChildren) {
        RECT gr{ rc.left + S(RowArrowX0(r)), rc.top,
                 rc.left + S(RowArrowX1(r)), nameBottom };
        COLORREF col = ThemeEnsureReadable(TC(5), TC(1));
        HFONT of = (HFONT)SelectObject(dc, g_glyphFont ? g_glyphFont : g_uiFont);
        SetTextColor(dc, col);
        const wchar_t* gl = r.expanded ? L"\u25BE" : L"\u25B8";
        DrawTextW(dc, gl, -1, &gr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, of);
    }

    std::wstring name = r.isParent ? L".." : r.node->name;
    if (rowIdx == 0 && pinnedRow_ == 0 && !r.isParent) name = pinLabel_;
    {
        static ULONGLONG s_lastDrawLog = 0;
        ULONGLONG dNow = GetTickCount64();
        if (dNow - s_lastDrawLog >= 800) {
            s_lastDrawLog = dNow;
            LogFl(L"[FL] draw row=" + std::to_wstring(rowIdx) + L" name=" +
                  (name.size() > 24 ? name.substr(0, 24) : name));
        }
    }
    SetTextColor(dc, r.isParent ? TC(4) : TC(0));
    RECT tr{ rc.left + S(RowNameX(r)), rc.top,
             rc.left + (int)ListView_GetColumnWidth(list_, 0) - S(4), nameBottom };
    DrawTextW(dc, name.c_str(), -1, &tr,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    // 其余四列
    ULONGLONG total = r.isParent ? (r.node->parent ? r.node->parent->size : 0)
                                 : (current_ ? current_->size : 0);
    int colX[6];
    int cx = 0;
    colX[0] = 0;
    for (int i = 0; i < 5; i++) {
        int cw = (int)ListView_GetColumnWidth(list_, i);
        cx += cw;
        colX[i + 1] = cx;
    }
    for (int c = 1; c < 5; c++) {
        std::wstring text;
        switch (c) {
            case 1: text = FormatSize(r.node->size); break;
            case 2:
                if (total) {
                    wchar_t b[32];
                    swprintf(b, 32, L"%.1f%%", (double)r.node->size / (double)total * 100.0);
                    text = b;
                }
                break;
            case 3: text = r.isParent ? L"上级目录" : TypeOfNode(r.node); break;
            case 4: text = FormatCount(r.node->fileCount); break;
        }
        RECT cr{ rc.left + colX[c] + S(6), rc.top, rc.left + colX[c + 1] - S(4), rc.bottom };
        SetTextColor(dc, TC(0));
        DrawTextW(dc, text.c_str(), -1, &cr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    SelectObject(dc, old);
}

void FileList::DrawUpRow(HDC dc, RECT rc) {
    HBRUSH bg = CreateSolidBrush(TC(6));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    HFONT old = (HFONT)SelectObject(dc, g_uiFont);
    SetBkMode(dc, TRANSPARENT);
    // 左侧画一个返回箭头样式
    COLORREF col = ThemeEnsureReadable(TC(4), TC(6));
    SetTextColor(dc, col);
    RECT ar{ rc.left + S(6), rc.top, rc.left + S(28), rc.bottom };
    DrawTextW(dc, L"⮝", -1, &ar, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    RECT tr{ rc.left + S(30), rc.top, rc.right - S(6), rc.bottom };
    std::wstring text = (current_ && current_->parent)
                            ? (std::wstring(L"返回上级：") + current_->name)
                            : L"返回上级";
    DrawTextW(dc, text.c_str(), -1, &tr,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, old);
}

// ------------------------------------------------------------ 交互 ----
void FileList::SelectRow(int idx) {
    if (idx < 0 || idx >= (int)rows_.size()) return;
    int old = selRow_;
    selRow_ = idx;
    auto redraw = [&](int r) {
        if (r >= 0 && r < (int)rowWnds_.size() && rowWnds_[r])
            InvalidateRect(rowWnds_[r], nullptr, TRUE);
    };
    redraw(old);
    redraw(idx);
    const Row& r = rows_[idx];
    if (!r.isParent && r.node && cb_.onSelect) cb_.onSelect(r.node);
}

void FileList::ToggleExpand(ScanNode* node) {
    if (!CanExpand(node)) return;
    if (expanded_.count(node)) expanded_.erase(node);
    else expanded_.insert(node);
    Rebuild();
    Refresh();
}

void FileList::HitTestRow(HWND hwnd, int row, POINT pt, bool dbl) {
    if (row < 0 || row >= (int)rows_.size()) return;
    Row r = EffectiveRow(row);
    if (!dbl) {
        int gx0 = S(RowArrowX0(r)) - S(8);
        int gx1 = S(RowArrowX1(r)) + S(8);
        if (r.hasChildren && !r.isParent && pt.x >= gx0 && pt.x <= gx1) {
            SelectRow(row);        // 点箭头也选中
            ToggleExpand(r.node);  // 只展开/折叠
            return;
        }
        SelectRow(row);
        return;
    }
    // 双击
    if (GetTickCount64() - lastWheelTick_ < 500) return;  // 滚轮误判
    SelectRow(row);
    if (r.isParent) {
        if (current_ && current_->parent) GoUp();
    } else if (r.node && r.node->isDir) {
        NavigateTo(r.node, true);
    } else if (r.node && cb_.onOpenFile) {
        cb_.onOpenFile(r.node);
    }
}

// ------------------------------------------------------- 窗口过程 ----
LRESULT CALLBACK FileList::RowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    FileList* self = (FileList*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_NCCREATE: {
            CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
            self = (FileList*)cs->lpCreateParams;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
            return TRUE;
        }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            int row = (int)((INT_PTR)GetWindowLongPtrW(hwnd, GWLP_ID) - 20000);
            static ULONGLONG s_lastPaintLog = 0;
            ULONGLONG pNow = GetTickCount64();
            if (pNow - s_lastPaintLog >= 800) {
                s_lastPaintLog = pNow;
                LogFl(L"[FL] row WM_PAINT row=" + std::to_wstring(row));
            }
            if (self && row >= 0 && row < self->Count())
                self->DrawRow(dc, rc, row, row == self->selRow_);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN:
            if (self) {
                int row = (int)((INT_PTR)GetWindowLongPtrW(hwnd, GWLP_ID) - 20000);
                POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                self->HitTestRow(hwnd, row, pt, false);
            }
            return 0;
        case WM_LBUTTONDBLCLK:
            if (self) {
                int row = (int)((INT_PTR)GetWindowLongPtrW(hwnd, GWLP_ID) - 20000);
                POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                self->HitTestRow(hwnd, row, pt, true);
            }
            return 0;
        case WM_RBUTTONUP:
            if (self) {
                int row = (int)((INT_PTR)GetWindowLongPtrW(hwnd, GWLP_ID) - 20000);
                if (row >= 0 && row < self->Count() && self->cb_.onContext) {
                    Row r = self->EffectiveRow(row);
                    if (r.node) {
                        self->SelectRow(row);
                        POINT sp{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                        ClientToScreen(hwnd, &sp);
                        self->cb_.onContext(r.node, sp);
                    }
                }
            }
            return 0;
        case WM_MOUSEWHEEL: {
            HWND lv = self ? self->list_ : nullptr;
            if (lv) SendMessageW(lv, WM_MOUSEWHEEL, wParam, lParam);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK FileList::UpRowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    FileList* self = (FileList*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_NCCREATE: {
            CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
            self = (FileList*)cs->lpCreateParams;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
            return TRUE;
        }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            int row = (int)((INT_PTR)GetWindowLongPtrW(hwnd, GWLP_ID) - 20000);
            static ULONGLONG s_lastPaintLog = 0;
            ULONGLONG pNow = GetTickCount64();
            if (pNow - s_lastPaintLog >= 800) {
                s_lastPaintLog = pNow;
                LogFl(L"[FL] row WM_PAINT row=" + std::to_wstring(row));
            }
            if (self && row >= 0 && row < self->Count())
                self->DrawRow(dc, rc, row, row == self->selRow_);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDBLCLK:
            if (self) self->GoUp();
            return 0;
        case WM_LBUTTONDOWN:
            if (self) self->GoUp();  // 单击也进入父级（资源管理器习惯）
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK FileList::ListProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                    UINT_PTR uId, DWORD_PTR refData) {
    FileList* self = (FileList*)refData;
    if (msg == WM_MOUSEWHEEL) {
        // 列表上的滚轮：正常交给 ListView 滚动（行窗口已转发到这里）
        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }
    if (msg == WM_VSCROLL || msg == WM_HSCROLL || msg == WM_MOUSEWHEEL ||
        msg == WM_KEYDOWN || msg == WM_CHAR) {
        // 列表滚动后立刻同步行窗口（否则行内容停在原地，滚回后看不到）
        LRESULT r = DefSubclassProc(hwnd, msg, wParam, lParam);
        if (self) self->OnListScroll((int)ListView_GetTopIndex(hwnd));
        return r;
    }
    if (msg == WM_LBUTTONDOWN) {
        // 防止 ListView 默认点击改变选中（行窗口层已接管行点击）
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

void FileList::ApplyTheme() {
    padX_ = TMetric(0);
    indent_ = TMetric(1);
    arrowBox_ = TMetric(2);
    if (host_) InvalidateRect(host_, nullptr, TRUE);
    if (upRow_) { InvalidateRect(upRow_, nullptr, TRUE); ShowWindow(upRow_, CanGoUp() ? SW_SHOW : SW_HIDE); }
    if (list_) {
        ListView_SetBkColor(list_, TC(1));
        ListView_SetTextColor(list_, TC(0));
        ListView_SetTextBkColor(list_, TC(1));
        InvalidateRect(list_, nullptr, TRUE);
    }
}
