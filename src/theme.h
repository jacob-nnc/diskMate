#pragma once
// ============================================================================
// theme.h — UI 外观层（与业务逻辑完全解耦）
//
// 设计目标：
//   1. 解耦：本文件只描述「长什么样」（颜色/尺寸/字体/圆角），不引用任何
//      扫描、树图、列表的业务类型。业务代码只通过 ThemeStore 取值。
//   2. 可配置：所有取值可由 JSON 覆盖（theme.json），未配置项回落到内置预设。
//      切换主题只改一份调色板数据，不需要改任何绘制代码。
//   3. 高可用：任何缺失/非法字段都回落到预设值，绝不因配置损坏而崩溃或不可读；
//      颜色值统一做可读性兜底（前景/背景对比度不足时自动纠正）。
// ============================================================================

#include <windows.h>

#include <string>
#include <vector>

#include "jsonlite.h"

// ---------------- 颜色 ----------------
struct ThemeColors {
    COLORREF bg = RGB(30, 34, 40);        // 窗口/列表背景
    COLORREF panel = RGB(38, 43, 51);     // 面板（列头、悬浮行、状态栏）
    COLORREF line = RGB(20, 23, 28);      // 分隔线
    COLORREF text = RGB(226, 232, 240);   // 主文字
    COLORREF textDim = RGB(150, 162, 178);// 次要文字
    COLORREF accent = RGB(45, 105, 255);  // 主操作
    COLORREF accentPressed = RGB(28, 84, 208);
    COLORREF danger = RGB(198, 68, 58);   // 危险操作
    COLORREF dangerPressed = RGB(150, 52, 44);
    COLORREF button = RGB(58, 64, 72);    // 次按钮
    COLORREF buttonHover = RGB(68, 75, 85);
    COLORREF buttonPressed = RGB(36, 41, 48);
    COLORREF onAccent = RGB(255, 255, 255);  // 强调色上的文字
    COLORREF selection = RGB(38, 82, 148);   // 选中行
    COLORREF track = RGB(52, 58, 68);        // 占比条轨道
    COLORREF arrow = RGB(150, 162, 178);     // 展开符号
    COLORREF treeBg = RGB(30, 34, 40);       // 树图画布
    COLORREF treeDir = RGB(64, 124, 210);    // 树图目录块
    COLORREF focusRing = RGB(255, 176, 64);// 焦点框
};

// ---------------- 尺寸（96dpi 逻辑像素） ----------------
struct ThemeMetrics {
    int toolbarH = 48;        // 顶部工具条高度
    int controlH = 26;        // 按钮/输入框高度
    int rowHeight = 24;       // 列表基础行高
    int rowExtra = 16;        // 行内附加行（大小·占比）高度；0 = 单行
    int radius = 8;           // 圆角半径
    int gap = 8;              // 通用间距
    int indent = 16;          // 树形缩进步长
    int arrowBox = 18;        // 展开符号热区宽度
    int titleFont = 13;       // 界面字号
    int smallFont = 11;       // 次要字号
    int labelFont = 12;       // 列表/树图块标签字号
    int padX = 8;             // 内容左内边距
};

// ---------------- 字体族 ----------------
struct ThemeFonts {
    std::wstring ui = L"Microsoft YaHei UI";
    std::wstring mono = L"Consolas";
};

// ---------------- 主题包 ----------------
struct Theme {
    std::wstring name;
    ThemeColors colors;
    ThemeMetrics metrics;
    ThemeFonts fonts;
};

// ---------------- 主题仓库（单例，全局唯一入口） ----------------
// 业务代码只依赖这个接口，不依赖具体主题内容。
class ThemeStore {
public:
    static ThemeStore& Instance();

    // 载入：先套用内置预设（dark/light），再用可选的 JSON 覆盖。
    // path 为空时只使用内置预设；文件缺失/损坏一律静默回落到预设。
    void Load(const std::wstring& path);

    // 切换预设：0=深色 1=浅色（保留 JSON 覆盖项）
    void SelectPreset(int preset);
    int Preset() const { return preset_; }

    const Theme& Get() const { return theme_; }

    // 便捷取色/取尺寸
    const ThemeColors& C() const { return theme_.colors; }
    const ThemeMetrics& M() const { return theme_.metrics; }
    const ThemeFonts& F() const { return theme_.fonts; }

    // 把主题导出为 JSON（便于「导出当前主题」与配置文件自举）
    std::wstring ToJson() const;

    // 配置路径（exe 同目录 theme.json，不可写时回落 %APPDATA%\DiskMate）
    static std::wstring DefaultPath();

private:
    ThemeStore() = default;
    void ApplyPreset(int preset);
    // 应用覆盖项；adoptPreset=true 时才采纳文件里的 preset 字段。
    void ApplyOverrides(const jsonlite::Value& root, bool adoptPreset);
    // 把当前 preset/name 写回文件，保留用户对颜色/尺寸的自定义。
    void PersistPreset(const std::wstring& path);

    Theme theme_;
    int preset_ = 0;         // 0=深色 1=浅色
    std::vector<std::pair<std::wstring, std::wstring>> overrides_;  // 原始覆盖项
};

// 全局快捷访问（与业务无关的纯外观取值）
inline const Theme& TH() { return ThemeStore::Instance().Get(); }
inline const ThemeColors& TC() { return ThemeStore::Instance().C(); }
inline const ThemeMetrics& TM() { return ThemeStore::Instance().M(); }
inline const ThemeFonts& TF() { return ThemeStore::Instance().F(); }

// ---------------- 对比度兜底 ----------------
// 相对亮度（sRGB 近似），用于判断前景/背景是否可读
double ThemeLuminance(COLORREF c);
// 若前景与背景对比不足，返回一个在该背景上可读的前景色；否则原样返回。
// 这是「高可用」的关键一环：无论用户把颜色配成什么，文字都保证看得见。
COLORREF ThemeEnsureReadable(COLORREF fg, COLORREF bg);
