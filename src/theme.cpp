#include "theme.h"

#include "jsonlite.h"

#include <shlobj.h>

#include <cmath>
#include <cstdio>

// ============================================================================
// 说明：本文件是「外观的唯一真源」。
//   - 业务代码（main/treemap/...）只读 ThemeStore，不硬编码颜色与尺寸。
//   - 修改外观：改预设、改 theme.json，或调 SelectPreset，都不需要动绘制代码。
//   - 健壮性：JSON 缺字段 → 用预设；值非法 → 夹到合法范围；对比度不足 → 自动纠正。
// ============================================================================

namespace {

// ---------- 内置预设 ----------
Theme MakeDark() {
    Theme t;
    t.name = L"dark";
    // 显式写全 metrics（不依赖隐式默认初始化，避免任何编译路径下取到 0）
    t.metrics.toolbarH = 48;
    t.metrics.controlH = 26;
    t.metrics.rowHeight = 24;
    t.metrics.rowExtra = 16;
    t.metrics.radius = 8;
    t.metrics.gap = 8;
    t.metrics.indent = 16;
    t.metrics.arrowBox = 18;
    t.metrics.titleFont = 13;
    t.metrics.smallFont = 11;
    t.metrics.labelFont = 12;
    t.metrics.padX = 8;
    ThemeColors& c = t.colors;
    c.bg = RGB(30, 34, 40);
    c.panel = RGB(38, 43, 51);
    c.line = RGB(20, 23, 28);
    c.text = RGB(226, 232, 240);
    c.textDim = RGB(150, 162, 178);
    c.accent = RGB(45, 105, 255);
    c.accentPressed = RGB(28, 84, 208);
    c.danger = RGB(198, 68, 58);
    c.dangerPressed = RGB(150, 52, 44);
    c.button = RGB(58, 64, 72);
    c.buttonHover = RGB(68, 75, 85);
    c.buttonPressed = RGB(36, 41, 48);
    c.onAccent = RGB(255, 255, 255);
    c.selection = RGB(38, 82, 148);
    c.track = RGB(52, 58, 68);
    c.arrow = RGB(150, 162, 178);
    c.treeBg = RGB(30, 34, 40);
    c.treeDir = RGB(64, 124, 210);
    c.focusRing = RGB(255, 176, 64);
    return t;
}

Theme MakeLight() {
    Theme t;
    t.name = L"light";
    t.metrics.toolbarH = 48;
    t.metrics.controlH = 26;
    t.metrics.rowHeight = 24;
    t.metrics.rowExtra = 16;
    t.metrics.radius = 8;
    t.metrics.gap = 8;
    t.metrics.indent = 16;
    t.metrics.arrowBox = 18;
    t.metrics.titleFont = 13;
    t.metrics.smallFont = 11;
    t.metrics.labelFont = 12;
    t.metrics.padX = 8;
    ThemeColors& c = t.colors;
    c.bg = RGB(248, 250, 252);
    c.panel = RGB(238, 241, 245);
    c.line = RGB(214, 220, 228);
    c.text = RGB(24, 30, 38);
    c.textDim = RGB(100, 110, 124);
    c.accent = RGB(37, 99, 235);
    c.accentPressed = RGB(29, 78, 190);
    c.danger = RGB(214, 74, 62);
    c.dangerPressed = RGB(172, 52, 42);
    c.button = RGB(233, 237, 242);
    c.buttonHover = RGB(222, 228, 236);
    c.buttonPressed = RGB(208, 215, 224);
    c.onAccent = RGB(255, 255, 255);
    c.selection = RGB(206, 226, 255);
    c.track = RGB(216, 222, 230);
    c.arrow = RGB(96, 106, 120);
    c.treeBg = RGB(246, 248, 251);
    c.treeDir = RGB(55, 115, 200);
    c.focusRing = RGB(230, 130, 20);
    return t;
}

// ---------- 颜色解析 ----------
// 支持 "#RRGGBB" / "#RGB" / "RRGGBB" / 十进制整数 0xRRGGBB
bool ParseColor(const std::wstring& in, COLORREF* out) {
    std::wstring s = in;
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t')) s.pop_back();
    if (s.empty()) return false;
    if (s[0] == L'#') s.erase(s.begin());
    if (s.size() == 3) {  // #RGB -> #RRGGBB
        std::wstring e;
        for (wchar_t ch : s) {
            e.push_back(ch);
            e.push_back(ch);
        }
        s = e;
    }
    if (s.size() == 6) {
        wchar_t* stop = nullptr;
        long v = wcstol(s.c_str(), &stop, 16);
        if (stop && *stop == 0) {
            *out = RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
            return true;
        }
        return false;
    }
    wchar_t* stop = nullptr;
    long v = wcstol(s.c_str(), &stop, 10);
    if (stop && *stop == 0 && v >= 0 && v <= 0xFFFFFF) {
        *out = RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
        return true;
    }
    return false;
}

int ClampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

double ThemeLuminance(COLORREF c) {
    auto ch = [](double v) {
        v /= 255.0;
        return v <= 0.03928 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * ch(GetRValue(c)) + 0.7152 * ch(GetGValue(c)) + 0.0722 * ch(GetBValue(c));
}

COLORREF ThemeEnsureReadable(COLORREF fg, COLORREF bg) {
    double l1 = ThemeLuminance(fg), l2 = ThemeLuminance(bg);
    double hi = l1 > l2 ? l1 : l2, lo = l1 > l2 ? l2 : l1;
    double ratio = (hi + 0.05) / (lo + 0.05);
    if (ratio >= 3.0) return fg;  // 足够可读
    // 不可读：按背景亮度决定推黑或推白
    return ThemeLuminance(bg) > 0.45 ? RGB(20, 24, 30) : RGB(245, 248, 252);
}

ThemeStore& ThemeStore::Instance() {
    static ThemeStore s;
    return s;
}

void ThemeStore::ApplyPreset(int preset) {
    preset_ = (preset == 1) ? 1 : 0;
    theme_ = (preset_ == 1) ? MakeLight() : MakeDark();
}

std::wstring ThemeStore::DefaultPath() {
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n && n < MAX_PATH) {
        std::wstring dir = exe;
        size_t pos = dir.find_last_of(L"\\/");
        if (pos != std::wstring::npos) dir = dir.substr(0, pos + 1);
        dir += L"theme.json";
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
        p += L"\\theme.json";
        return p;
    }
    return L"theme.json";
}

// 应用覆盖项；adoptPreset=true 时才采纳文件里的 preset 字段。
// 显式切换主题时传 false，避免「刚设为浅色又被配置文件的旧 preset 打回去」。
void ThemeStore::ApplyOverrides(const jsonlite::Value& root, bool adoptPreset) {
    if (adoptPreset && root.has(L"preset")) {
        int p = root.child(L"preset").asInt(preset_);
        if (p != preset_) ApplyPreset(p);
    }
    if (root.has(L"name")) {
        std::wstring nm = root.child(L"name").asStr(theme_.name);
        if (!nm.empty()) theme_.name = nm;
    }

    // colors：逐字段覆盖，非法值忽略
    const jsonlite::Value& cols = root.child(L"colors");
    if (cols.kind == jsonlite::Value::OBJ) {
        struct ColorSpec {
            const wchar_t* key;
            COLORREF* dst;
        };
        ColorSpec specs[] = {
            { L"bg", &theme_.colors.bg },
            { L"panel", &theme_.colors.panel },
            { L"line", &theme_.colors.line },
            { L"text", &theme_.colors.text },
            { L"textDim", &theme_.colors.textDim },
            { L"accent", &theme_.colors.accent },
            { L"accentPressed", &theme_.colors.accentPressed },
            { L"danger", &theme_.colors.danger },
            { L"dangerPressed", &theme_.colors.dangerPressed },
            { L"button", &theme_.colors.button },
            { L"buttonHover", &theme_.colors.buttonHover },
            { L"buttonPressed", &theme_.colors.buttonPressed },
            { L"onAccent", &theme_.colors.onAccent },
            { L"selection", &theme_.colors.selection },
            { L"track", &theme_.colors.track },
            { L"arrow", &theme_.colors.arrow },
            { L"treeBg", &theme_.colors.treeBg },
            { L"treeDir", &theme_.colors.treeDir },
            { L"focusRing", &theme_.colors.focusRing },
        };
        for (auto& sp : specs) {
            if (!cols.has(sp.key)) continue;
            COLORREF c;
            if (ParseColor(cols.child(sp.key).asStr(L""), &c)) {
                *sp.dst = c;
                overrides_.push_back({ sp.key, cols.child(sp.key).asStr(L"") });
            }
        }
    }

    // metrics：数值夹到合法范围（防止配置把行高设成 0 导致界面不可用）
    const jsonlite::Value& m = root.child(L"metrics");
    if (m.kind == jsonlite::Value::OBJ) {
        struct IntSpec {
            const wchar_t* key;
            int* dst;
            int lo, hi;
        };
        IntSpec specs[] = {
            { L"toolbarH", &theme_.metrics.toolbarH, 28, 200 },
            { L"controlH", &theme_.metrics.controlH, 18, 80 },
            { L"rowHeight", &theme_.metrics.rowHeight, 14, 120 },
            { L"rowExtra", &theme_.metrics.rowExtra, 0, 48 },
            { L"radius", &theme_.metrics.radius, 0, 24 },
            { L"gap", &theme_.metrics.gap, 0, 48 },
            { L"indent", &theme_.metrics.indent, 4, 64 },
            { L"arrowBox", &theme_.metrics.arrowBox, 10, 48 },
            { L"titleFont", &theme_.metrics.titleFont, 9, 32 },
            { L"smallFont", &theme_.metrics.smallFont, 8, 32 },
            { L"labelFont", &theme_.metrics.labelFont, 8, 32 },
            { L"padX", &theme_.metrics.padX, 0, 64 },
        };
        for (auto& sp : specs) {
            if (!m.has(sp.key)) continue;
            int v = m.child(sp.key).asInt(*sp.dst);
            *sp.dst = ClampInt(v, sp.lo, sp.hi);
        }
    }

    // fonts
    const jsonlite::Value& f = root.child(L"fonts");
    if (f.kind == jsonlite::Value::OBJ) {
        if (f.has(L"ui")) theme_.fonts.ui = f.child(L"ui").asStr(theme_.fonts.ui);
        if (f.has(L"mono")) theme_.fonts.mono = f.child(L"mono").asStr(theme_.fonts.mono);
    }

    // 可读性兜底：文字类颜色若在其背景上不可读，自动纠正
    theme_.colors.text = ThemeEnsureReadable(theme_.colors.text, theme_.colors.bg);
    theme_.colors.textDim = ThemeEnsureReadable(theme_.colors.textDim, theme_.colors.bg);
    theme_.colors.onAccent = ThemeEnsureReadable(theme_.colors.onAccent, theme_.colors.accent);
    theme_.colors.arrow = ThemeEnsureReadable(theme_.colors.arrow, theme_.colors.bg);
}

void ThemeStore::Load(const std::wstring& path) {
    bool first = theme_.name.empty();
    if (first) ApplyPreset(0);  // 首次：先给深色预设
    std::wstring p = path.empty() ? DefaultPath() : path;
    std::wstring text = jsonlite::ReadFile(p);
    if (text.empty()) {
        // 没有主题文件：写一份当前预设，方便用户直接改
        jsonlite::WriteFile(p, ToJson());
        return;
    }
    jsonlite::Value root = jsonlite::Parse(text);
    if (root.kind != jsonlite::Value::OBJ) return;  // 损坏 → 保留预设
    overrides_.clear();
    // 仅首次初始化时采纳文件里的 preset；之后的显式切换以调用方为准
    ApplyOverrides(root, /*adoptPreset=*/first);
}

void ThemeStore::SelectPreset(int preset) {
    std::wstring path = DefaultPath();
    ApplyPreset(preset);
    // 套用 color/metrics/fonts 覆盖项，但忽略文件里的 preset（以本次调用为准）
    std::wstring text = jsonlite::ReadFile(path);
    if (!text.empty()) {
        jsonlite::Value root = jsonlite::Parse(text);
        if (root.kind == jsonlite::Value::OBJ) {
            overrides_.clear();
            ApplyOverrides(root, /*adoptPreset=*/false);
        }
    }
    // 把当前预设写回文件（保证再次启动仍是用户选的主题）
    PersistPreset(path);
}

// 只更新文件里的 preset/name 两个字段，保留用户对颜色/尺寸的自定义。
// 注意：颜色/尺寸只在用户显式配置过时才写入；否则文件里只留 preset，
// 避免「把预设色当覆盖项落盘 → 下次切预设又被这些旧值盖回去」。
void ThemeStore::PersistPreset(const std::wstring& path) {
    jsonlite::Value root = jsonlite::MakeObj();
    root.obj[L"preset"] = jsonlite::MakeInt(preset_);
    root.obj[L"name"] = jsonlite::MakeStr(theme_.name);
    jsonlite::Value c = jsonlite::MakeObj();
    for (const auto& kv : overrides_) {
        // 只把用户真正自定义过的字段写回（值为合法颜色串）
        c.obj[kv.first] = jsonlite::MakeStr(kv.second);
    }
    if (!overrides_.empty()) root.obj[L"colors"] = c;
    jsonlite::WriteFile(path, jsonlite::Dump(root));
}


std::wstring ThemeStore::ToJson() const {
    const Theme& t = theme_;
    auto col = [](COLORREF c) {
        wchar_t buf[16];
        swprintf(buf, 16, L"#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
        return std::wstring(buf);
    };
    jsonlite::Value root = jsonlite::MakeObj();
    root.obj[L"name"] = jsonlite::MakeStr(t.name);
    root.obj[L"preset"] = jsonlite::MakeInt(preset_);

    jsonlite::Value c = jsonlite::MakeObj();
    c.obj[L"bg"] = jsonlite::MakeStr(col(t.colors.bg));
    c.obj[L"panel"] = jsonlite::MakeStr(col(t.colors.panel));
    c.obj[L"line"] = jsonlite::MakeStr(col(t.colors.line));
    c.obj[L"text"] = jsonlite::MakeStr(col(t.colors.text));
    c.obj[L"textDim"] = jsonlite::MakeStr(col(t.colors.textDim));
    c.obj[L"accent"] = jsonlite::MakeStr(col(t.colors.accent));
    c.obj[L"accentPressed"] = jsonlite::MakeStr(col(t.colors.accentPressed));
    c.obj[L"danger"] = jsonlite::MakeStr(col(t.colors.danger));
    c.obj[L"dangerPressed"] = jsonlite::MakeStr(col(t.colors.dangerPressed));
    c.obj[L"button"] = jsonlite::MakeStr(col(t.colors.button));
    c.obj[L"buttonHover"] = jsonlite::MakeStr(col(t.colors.buttonHover));
    c.obj[L"buttonPressed"] = jsonlite::MakeStr(col(t.colors.buttonPressed));
    c.obj[L"onAccent"] = jsonlite::MakeStr(col(t.colors.onAccent));
    c.obj[L"selection"] = jsonlite::MakeStr(col(t.colors.selection));
    c.obj[L"track"] = jsonlite::MakeStr(col(t.colors.track));
    c.obj[L"arrow"] = jsonlite::MakeStr(col(t.colors.arrow));
    c.obj[L"treeBg"] = jsonlite::MakeStr(col(t.colors.treeBg));
    c.obj[L"treeDir"] = jsonlite::MakeStr(col(t.colors.treeDir));
    c.obj[L"focusRing"] = jsonlite::MakeStr(col(t.colors.focusRing));
    root.obj[L"colors"] = c;

    jsonlite::Value m = jsonlite::MakeObj();
    m.obj[L"toolbarH"] = jsonlite::MakeInt(t.metrics.toolbarH);
    m.obj[L"controlH"] = jsonlite::MakeInt(t.metrics.controlH);
    m.obj[L"rowHeight"] = jsonlite::MakeInt(t.metrics.rowHeight);
    m.obj[L"rowExtra"] = jsonlite::MakeInt(t.metrics.rowExtra);
    m.obj[L"radius"] = jsonlite::MakeInt(t.metrics.radius);
    m.obj[L"gap"] = jsonlite::MakeInt(t.metrics.gap);
    m.obj[L"indent"] = jsonlite::MakeInt(t.metrics.indent);
    m.obj[L"arrowBox"] = jsonlite::MakeInt(t.metrics.arrowBox);
    m.obj[L"titleFont"] = jsonlite::MakeInt(t.metrics.titleFont);
    m.obj[L"smallFont"] = jsonlite::MakeInt(t.metrics.smallFont);
    m.obj[L"labelFont"] = jsonlite::MakeInt(t.metrics.labelFont);
    m.obj[L"padX"] = jsonlite::MakeInt(t.metrics.padX);
    root.obj[L"metrics"] = m;

    jsonlite::Value f = jsonlite::MakeObj();
    f.obj[L"ui"] = jsonlite::MakeStr(t.fonts.ui);
    f.obj[L"mono"] = jsonlite::MakeStr(t.fonts.mono);
    root.obj[L"fonts"] = f;

    return jsonlite::Dump(root);
}
