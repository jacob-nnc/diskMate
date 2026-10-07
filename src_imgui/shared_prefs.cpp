// ============================================================================
// DiskMate ImGui 前端 —— 共用偏好存储（单后端 / 多前端）
//
// 三个前端（WebView2 / ImGui / 原生 Win32）共用 exe 同目录下的这几个文件：
//   config.json    —— settings.cpp 的 AppSettings（扫描/树图 6 项/界面 3 项/18 项右键菜单）
//                     三个前端都读写；设置面板的「保存」写它
//   ui-prefs.json  —— **页面拥有的界面偏好 blob**（UTF-8、无 BOM），Web 版写的就是它：
//                     theme / mapKind / ordKind / powAlpha / extMap / extFallback / cols
//                     ImGui 侧按同一 schema 读写，并保留自己不认识的键（不丢 Web 的设置）
//   diskmate.ini   —— Web 宿主的 key=value 简单设置：lastPath / mft
//
// 兼容策略：theme / mapKind / ordKind / powAlpha 这四个标量在两边都有位置
//   · 读：以 ui-prefs.json 为准（Web 版的真源），缺失时保留 config.json 的值
//   · 写：两个文件都写（原生 Win32 前端仍从 config.json 读，保持三者一致）
// ============================================================================
#include "dm.h"
#include "jsonlite.h"

#include <cstdio>

using namespace dm;

namespace prefs {

// ---------------------------------------------------------------- 路径 ----
static std::wstring ExeDirP() {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring d = exe;
    size_t sl = d.find_last_of(L"\\/");
    return (sl == std::wstring::npos) ? L"" : d.substr(0, sl + 1);
}
std::wstring UiPrefsPath() { return ExeDirP() + L"ui-prefs.json"; }
std::wstring IniPath() { return ExeDirP() + L"diskmate.ini"; }

// ------------------------------------------------------- UTF-8 文件读写 ----
// ui-prefs.json 必须是 UTF-8 无 BOM（Web 宿主的读写方式），不能用 jsonlite 的 UTF-16 写
static std::wstring ReadUtf8File(const std::wstring& path) {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"rb") != 0 || !fp) return L"";
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    std::wstring out;
    if (n > 0) {
        std::string buf((size_t)n, '\0');
        fread(&buf[0], 1, (size_t)n, fp);
        // BOM 处理：UTF-8 BOM 跳过；UTF-16 BOM 按 UTF-16 解
        if (n >= 2 && (unsigned char)buf[0] == 0xFF && (unsigned char)buf[1] == 0xFE) {
            out.assign((const wchar_t*)(buf.data() + 2), (size_t)((n - 2) / 2));
        } else {
            size_t off = 0;
            if (n >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB &&
                (unsigned char)buf[2] == 0xBF)
                off = 3;
            int wl = MultiByteToWideChar(CP_UTF8, 0, buf.data() + off, (int)((size_t)n - off),
                                         nullptr, 0);
            out.resize((size_t)(wl > 0 ? wl : 0));
            if (wl > 0)
                MultiByteToWideChar(CP_UTF8, 0, buf.data() + off, (int)((size_t)n - off), &out[0],
                                    wl);
        }
    }
    fclose(fp);
    return out;
}
static bool WriteUtf8File(const std::wstring& path, const std::wstring& text) {
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr,
                                nullptr);
    std::string buf((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &buf[0], n, nullptr,
                            nullptr);
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"wb") != 0 || !fp) return false;
    fwrite(buf.data(), 1, buf.size(), fp);
    fclose(fp);
    return true;
}

// -------------------------------------------------------- key <-> 数值 ----
static const wchar_t* MapKindKey(int k) {
    switch (k) {
        case 1: return L"log";
        case 2: return L"log2";
        case 3: return L"sqrt";
        case 4: return L"pow";
        default: return L"id";
    }
}
static int MapKindFrom(const std::wstring& s) {
    if (s == L"log") return 1;
    if (s == L"log2") return 2;
    if (s == L"sqrt") return 3;
    if (s == L"pow") return 4;
    return 0;
}
static const wchar_t* OrdKindKey(int k) { return (k == 1) ? L"Gsum" : L"sumG"; }
static int OrdKindFrom(const std::wstring& s) { return (s == L"Gsum") ? 1 : 0; }

// 短键 → 中文分组名（与 web/index.html 的 EXT_GROUP_NAMES 一致；未知键原样显示）
struct NameKey { const wchar_t* key; const wchar_t* name; };
static const NameKey kNames[] = {
    { L"img", L"图片" },     { L"vid", L"视频" },   { L"aud", L"音频" },
    { L"doc", L"文档" },     { L"zip", L"压缩" },   { L"code", L"代码" },
    { L"exe", L"可执行" },   { L"lib", L"库/系统" }, { L"vdisk", L"虚拟磁盘" },
    { L"log", L"日志/配置" },
};
static std::wstring NameOfKey(const std::wstring& key) {
    for (const auto& n : kNames)
        if (key == n.key) return n.name;
    return key;
}

static ImU32 ParseHex(const std::wstring& s, ImU32 def) {
    if (s.size() < 7 || s[0] != L'#') return def;
    auto hex = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return -1;
    };
    int v = 0;
    for (int i = 1; i <= 6; i++) {
        int h = hex(s[(size_t)i]);
        if (h < 0) return def;
        v = (v << 4) | h;
    }
    return IM_COL32((v >> 16) & 255, (v >> 8) & 255, v & 255, 255);
}
static std::wstring HexOf(ImU32 c) {
    wchar_t b[16];
    swprintf(b, 16, L"#%02x%02x%02x", (unsigned)((c >> IM_COL32_R_SHIFT) & 255),
             (unsigned)((c >> IM_COL32_G_SHIFT) & 255), (unsigned)((c >> IM_COL32_B_SHIFT) & 255));
    return b;
}

// 解析后的整份文档（保存时只改自己的键，其余原样写回）
static jsonlite::Value s_doc;
static bool s_docLoaded = false;

// ------------------------------------------------------------ 载入 ----
void LoadUiPrefs() {
    std::wstring text = ReadUtf8File(UiPrefsPath());
    s_doc = text.empty() ? jsonlite::MakeObj() : jsonlite::Parse(text);
    if (s_doc.kind != jsonlite::Value::OBJ) s_doc = jsonlite::MakeObj();
    s_docLoaded = true;

    std::wstring th = s_doc.child(L"theme").asStr(L"");
    if (th == L"light") g_st.theme = 1;
    else if (th == L"dark") g_st.theme = 0;

    if (s_doc.has(L"mapKind")) g_st.mapKind = MapKindFrom(s_doc.child(L"mapKind").asStr(L""));
    if (s_doc.has(L"ordKind")) g_st.ordKind = OrdKindFrom(s_doc.child(L"ordKind").asStr(L""));
    if (s_doc.has(L"powAlpha")) {
        double a = s_doc.child(L"powAlpha").asNum(g_st.powAlpha);
        if (a >= 0.1 && a <= 1.0) g_st.powAlpha = a;
    }

    const jsonlite::Value& em = s_doc.child(L"extMap");
    if (em.kind == jsonlite::Value::OBJ && !em.obj.empty()) {
        std::vector<ExtGroup> ng;
        for (const auto& kv : em.obj) {
            ExtGroup g;
            g.key = kv.first;
            g.name = NameOfKey(kv.first);
            g.color = ParseHex(kv.second.child(L"color").asStr(L""), IM_COL32(0x5a, 0x64, 0x72, 255));
            for (const auto& e : kv.second.child(L"exts").arr) {
                std::wstring x = e.asStr(L"");
                std::transform(x.begin(), x.end(), x.begin(), ::towlower);
                if (!x.empty()) g.exts.push_back(x);
            }
            ng.push_back(std::move(g));
        }
        if (!ng.empty()) g_extMap = std::move(ng);
    }
    if (s_doc.has(L"extFallback"))
        g_extFallback = ParseHex(s_doc.child(L"extFallback").asStr(L""), g_extFallback);

    const jsonlite::Value& cols = s_doc.child(L"cols");
    if (cols.kind == jsonlite::Value::OBJ) {
        g_nameW = (float)cols.child(L"nameW").asNum(g_nameW);
        g_splitFrac = (float)cols.child(L"split").asNum(g_splitFrac);
        const wchar_t* keys[4] = { L"size", L"pct", L"type", L"files" };
        for (int i = 0; i < 4; i++) g_colW[i] = (float)cols.child(keys[i]).asNum(g_colW[i]);
        g_nameW = std::max(NAME_MIN, g_nameW);
        g_splitFrac = std::min(SPLIT_MAX, std::max(SPLIT_MIN, g_splitFrac));
        const float mins[4] = { 56, 48, 56, 56 };
        for (int i = 0; i < 4; i++) g_colW[i] = std::max(mins[i], g_colW[i]);
    }
    LogLine(L"ui-prefs loaded theme=" + std::to_wstring(g_st.theme) + L" mapKind=" +
            std::to_wstring(g_st.mapKind) + L" ordKind=" + std::to_wstring(g_st.ordKind) +
            L" extGroups=" + std::to_wstring(g_extMap.size()));
}

// ------------------------------------------------------------ 保存 ----
void SaveUiPrefs() {
    if (!s_docLoaded) {
        s_doc = jsonlite::MakeObj();
        s_docLoaded = true;
    }
    auto& o = s_doc.obj;
    o[L"theme"] = jsonlite::MakeStr(g_st.theme == 1 ? L"light" : L"dark");
    o[L"mapKind"] = jsonlite::MakeStr(MapKindKey(g_st.mapKind));
    o[L"ordKind"] = jsonlite::MakeStr(OrdKindKey(g_st.ordKind));
    o[L"powAlpha"] = jsonlite::MakeNum(g_st.powAlpha);

    jsonlite::Value em = jsonlite::MakeObj();
    for (const auto& g : g_extMap) {
        jsonlite::Value one = jsonlite::MakeObj();
        one.obj[L"color"] = jsonlite::MakeStr(HexOf(g.color));
        jsonlite::Value ea = jsonlite::MakeObj();
        ea.kind = jsonlite::Value::ARR;
        for (const auto& e : g.exts) ea.arr.push_back(jsonlite::MakeStr(e));
        one.obj[L"exts"] = ea;
        em.obj[g.key.empty() ? g.name : g.key] = one;
    }
    o[L"extMap"] = em;
    o[L"extFallback"] = jsonlite::MakeStr(HexOf(g_extFallback));

    jsonlite::Value cols = jsonlite::MakeObj();
    cols.obj[L"nameW"] = jsonlite::MakeNum(g_nameW);
    cols.obj[L"split"] = jsonlite::MakeNum(g_splitFrac);
    const wchar_t* keys[4] = { L"size", L"pct", L"type", L"files" };
    for (int i = 0; i < 4; i++) cols.obj[keys[i]] = jsonlite::MakeNum(g_colW[i]);
    o[L"cols"] = cols;

    WriteUtf8File(UiPrefsPath(), jsonlite::Dump(s_doc));
    // 四个标量同步进 config.json：原生 Win32 前端仍从那里读主题/面积算法
    SaveSettings(g_st);
}

// --------------------------------------------------- diskmate.ini ----
std::wstring IniGet(const wchar_t* key, const std::wstring& def) {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, IniPath().c_str(), L"rb") != 0 || !fp) return def;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    std::wstring text;
    if (n > 0) {
        std::string buf((size_t)n, '\0');
        fread(&buf[0], 1, (size_t)n, fp);
        size_t off = (n >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB &&
                      (unsigned char)buf[2] == 0xBF)
                         ? 3
                         : 0;
        int wl = MultiByteToWideChar(CP_UTF8, 0, buf.data() + off, (int)((size_t)n - off), nullptr,
                                     0);
        text.resize((size_t)(wl > 0 ? wl : 0));
        if (wl > 0)
            MultiByteToWideChar(CP_UTF8, 0, buf.data() + off, (int)((size_t)n - off), &text[0], wl);
    }
    fclose(fp);
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t eol = text.find(L'\n', pos);
        if (eol == std::wstring::npos) eol = text.size();
        std::wstring line = text.substr(pos, eol - pos);
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
        size_t eq = line.find(L'=');
        if (eq != std::wstring::npos && eq > 0 && line.compare(0, eq, key) == 0)
            return line.substr(eq + 1);
        if (eol == text.size()) break;
        pos = eol + 1;
    }
    return def;
}

void IniSet(const wchar_t* key, const std::wstring& value) {
    // 读全部 → 改一项 → 写全部（与 Web 宿主同款，避免写一项清掉其它项）
    std::vector<std::pair<std::wstring, std::wstring>> kv;
    {
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, IniPath().c_str(), L"rb") == 0 && fp) {
            fseek(fp, 0, SEEK_END);
            long n = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            if (n > 0) {
                std::string buf((size_t)n, '\0');
                fread(&buf[0], 1, (size_t)n, fp);
                size_t off = (n >= 3 && (unsigned char)buf[0] == 0xEF &&
                              (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF)
                                 ? 3
                                 : 0;
                int wl = MultiByteToWideChar(CP_UTF8, 0, buf.data() + off,
                                             (int)((size_t)n - off), nullptr, 0);
                std::wstring text((size_t)(wl > 0 ? wl : 0), L'\0');
                if (wl > 0)
                    MultiByteToWideChar(CP_UTF8, 0, buf.data() + off, (int)((size_t)n - off),
                                        &text[0], wl);
                size_t pos = 0;
                while (pos <= text.size()) {
                    size_t eol = text.find(L'\n', pos);
                    if (eol == std::wstring::npos) eol = text.size();
                    std::wstring line = text.substr(pos, eol - pos);
                    while (!line.empty() && (line.back() == L'\r' || line.back() == L' '))
                        line.pop_back();
                    size_t eq = line.find(L'=');
                    if (eq != std::wstring::npos && eq > 0)
                        kv.emplace_back(line.substr(0, eq), line.substr(eq + 1));
                    if (eol == text.size()) break;
                    pos = eol + 1;
                }
            }
            fclose(fp);
        }
    }
    bool found = false;
    for (auto& p : kv) {
        if (p.first == key) { p.second = value; found = true; break; }
    }
    if (!found) kv.emplace_back(key, value);
    std::wstring out;
    for (const auto& p : kv) {
        out += p.first;
        out += L'=';
        out += p.second;
        out += L"\r\n";
    }
    WriteUtf8File(IniPath(), out);
}

}  // namespace prefs
