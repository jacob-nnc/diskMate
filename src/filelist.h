#pragma once
// ============================================================================
// FileList —— 左侧文件列表管理器（整类封装，替代原先散在 main.cpp 的行窗口层）
//   行数据 / 展开态 / 选中 / 悬浮父行 / 滚动 / 历史 / 自绘 / 命中 全部内聚。
// ============================================================================
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <unordered_set>
#include <functional>
#include "scanner.h"
#include "settings.h"

class FileList {
public:
    // 一行（扁平化：展开的子项内联带缩进）
    struct Row {
        bool isParent = false;    // 固定「..」父级行
        bool hasChildren = false; // 是目录且可展开
        bool expanded = false;    // 当前展开
        int depth = 0;            // 缩进层级（相对视图顶层）
        ScanNode* node = nullptr;
    };

    // 交互回调（由 main.cpp 注入）
    struct Callbacks {
        std::function<void(ScanNode*)> onEnterDir;        // 双击目录 / 「..」进入
        std::function<void(ScanNode*)> onOpenFile;        // 双击文件打开
        std::function<void(ScanNode*, POINT)> onContext;  // 右键菜单（屏幕坐标）
        std::function<void(ScanNode*)> onSelect;          // 选中联动树图
    };

    FileList() = default;
    ~FileList();

    void Create(HWND parent, HINSTANCE inst, const AppSettings* settings);
    void SetCallbacks(Callbacks cb) { cb_ = std::move(cb); }
    void ApplyTheme();                      // 主题变化后整体刷新

    // ---- 树 / 导航 ----
    void SetCurrent(ScanNode* node, const std::vector<ScanNode*>& view);
    void ResetScan();   // 新扫描前清空全部列表状态（展开/历史/行/选中/滚动） // 导航到新目录
    void NavigateTo(ScanNode* node, bool pushHistory);
    void GoUp();                            // 进入父级
    void GoBack();
    void GoForward();
    ScanNode* Current() const { return current_; }
    const std::vector<ScanNode*>& View() const { return view_; }
    bool CanGoUp() const { return current_ && current_->parent; }
    bool CanBack() const { return !back_.empty(); }
    bool CanForward() const { return !fwd_.empty(); }

    // ---- 排序（默认大小降序） ----
    void SetSort(int col, bool asc);        // col: 0=名称 1=大小 2=占比 3=类型 4=文件数
    void SortView();

    // ---- 行管理 ----
    void Rebuild();                         // 展开态 / 排序 / 导航后重建可见行
    void Refresh();                         // 同步列表项数 + 行窗口布局 + 悬浮
    void Layout(int x, int y, int w, int h, int upH); // 由外部 WM_SIZE 调用
    void SelectRow(int idx);
    void ToggleExpand(ScanNode* node);
    void OnListScroll(int topIndex);        // ListView 滚动同步

    // ---- 查询 ----
    int Count() const { return (int)rows_.size(); }
    const Row& RowAt(int idx) const;
    ScanNode* SelNode() const;
    int SelRow() const { return selRow_; }
    bool CanExpandP(const ScanNode* n) const { return CanExpand(n); }
    const std::wstring& PinLabel() const { return pinLabel_; }
    bool Pinned() const { return pinnedRow_ == 0; }
    HWND ListHwnd() const { return list_; }
    HWND UpRowHwnd() const { return upRow_; }
    const std::unordered_set<ScanNode*>& Expanded() const { return expanded_; }
    void ClearExpanded() { expanded_.clear(); }

    // 布局几何（供 main.cpp 状态栏诊断）
    int RowHeight() const { return rowH_; }
    int ScrollY() const { return scrollY_; }

private:
    static LRESULT CALLBACK ListProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static LRESULT CALLBACK RowProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK UpRowProc(HWND, UINT, WPARAM, LPARAM);

    // 内部工具
    int RowHeightPx() const;
    bool CanExpand(const ScanNode* n) const;
    std::vector<ScanNode*> SortedChildren(ScanNode* dir);
    void EnsureRowWnds(int need);
    void PlaceRows();                       // 按滚动偏移摆放行窗口（可视区+缓冲）
    void DrawRow(HDC dc, RECT rc, int rowIdx, bool selected);
    void DrawUpRow(HDC dc, RECT rc);
    void UpdatePin();
    void HitTestRow(HWND hwnd, int row, POINT pt, bool dbl);
    Row EffectiveRow(int idx) const;
    int RowIndentPx(int depth) const { return padX_ + depth * indent_; }
    int RowArrowX0(const Row& r) const { return RowIndentPx(r.isParent ? 0 : r.depth); }
    int RowArrowX1(const Row& r) const { return RowArrowX0(r) + arrowBox_; }
    int RowNameX(const Row& r) const {
        if (r.isParent) return padX_;
        return RowIndentPx(r.depth) + arrowBox_ + 4;
    }
    static std::wstring FormatSize(ULONGLONG b);
    static std::wstring FormatCount(ULONGLONG n);

    // 数据
    const AppSettings* st_ = nullptr;
    HINSTANCE inst_ = nullptr;
    HWND parent_ = nullptr;
    HWND list_ = nullptr;      // ListView（滚动容器）
    HWND host_ = nullptr;      // 行容器（盖住列表客户区，承载每行自绘窗口）
    HWND upRow_ = nullptr;     // 固定「..」行
    HWND header_ = nullptr;    // 表头（仅用于测高）
    int padX_ = 8, indent_ = 16, arrowBox_ = 18;

    std::vector<Row> rows_;
    std::vector<int> rowView_;            // 每行对应 view_ 下标；-1 = 子项/父级
    std::unordered_set<ScanNode*> expanded_;
    std::vector<ScanNode*> view_;
    ScanNode* current_ = nullptr;
    std::vector<ScanNode*> back_, fwd_;

    int selRow_ = -1;
    int pinnedRow_ = -1;                  // 悬浮父行显示在首行（0 或 -1）
    ScanNode* pinNode_ = nullptr;
    std::wstring pinLabel_;
    int scrollY_ = 0;
    int rowH_ = 0;
    int sortCol_ = 1;                     // 默认大小
    bool sortAsc_ = false;                // 默认降序

    std::vector<HWND> rowWnds_;
    HIMAGELIST rowImg_ = nullptr;         // 强制 ListView 行高与行窗口一致
    Callbacks cb_;
    ULONGLONG lastWheelTick_ = 0;
    bool skipNextClick_ = false;
};
