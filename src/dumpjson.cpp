// dumpjson —— 把指定目录扫描成 JSON 写文件（控制台工具，无 UI）
//
// 用法:
//   dumpjson <输出目录或文件> <路径1> [路径2] ...
//
// 例:
//   dumpjson D:\Desktop\dump C:\ D:\
//       → D:\Desktop\dump\C.json 和 D:\Desktop\dump\D.json
//   dumpjson D:\Desktop\dump\all.json C:\ D:\
//       → 两个盘合并写进一个 all.json
//
// JSON 结构（每个根一棵树，完整字段，depth 不限）:
//   {
//     "roots": [
//       { "name":"C:\", "path":"C:\", "size":123, "isDir":true,
//         "fileCount":10, "dirCount":5, "children":[ ... ] }
//     ],
//     "generatedAt": "2026-10-05T18:20:00"
//   }
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>
#include "scanner.h"

static std::string W2U8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// JSON 字符串转义（含控制字符）
static std::string JStr(const std::wstring& w) {
    std::string s = W2U8(w), o;
    o.reserve(s.size() + 2);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            case '\b': o += "\\b";  break;
            case '\f': o += "\\f";  break;
            default:
                if (c < 0x20) { char buf[8]; sprintf(buf, "\\u%04x", c); o += buf; }
                else o += (char)c;
        }
    }
    return o;
}

// 递归输出节点（含全部后代，无深度/数量限制）
static void NodeToJson(const ScanNode* n, std::string& out) {
    out += "{";
    out += "\"name\":\"" + JStr(n->name) + "\"";
    out += ",\"path\":\"" + JStr(NodeFullPath(n)) + "\"";
    out += ",\"size\":" + std::to_string(n->size);
    out += ",\"isDir\":" + std::string(n->isDir ? "true" : "false");
    out += ",\"fileCount\":" + std::to_string(n->fileCount);
    out += ",\"dirCount\":" + std::to_string(n->children.size());
    if (!n->children.empty()) {
        // 子项按大小降序（和界面一致，方便人看）
        std::vector<const ScanNode*> kids(n->children.begin(), n->children.end());
        std::stable_sort(kids.begin(), kids.end(),
                         [](const ScanNode* a, const ScanNode* b) { return a->size > b->size; });
        out += ",\"children\":[";
        for (size_t i = 0; i < kids.size(); i++) {
            if (i) out += ",";
            NodeToJson(kids[i], out);
        }
        out += "]";
    }
    out += "}";
}

static void Print(const char* s) { fputs(s, stderr); fflush(stderr); }

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        fprintf(stderr,
            "用法:\n"
            "  dumpjson <输出目录或文件> <路径1> [路径2] ...\n\n"
            "例:\n"
            "  dumpjson D:\\dump C:\\ D:\\\n"
            "  dumpjson D:\\dump\\all.json C:\\ D:\\\n");
        return 2;
    }

    std::wstring outArg = argv[1];
    std::vector<std::wstring> roots;
    for (int i = 2; i < argc; i++) {
        std::wstring p = argv[i];
        // 去掉可能的引号
        while (!p.empty() && (p.front() == L'"' || p.front() == L'\'')) p.erase(p.begin());
        while (!p.empty() && (p.back()  == L'"' || p.back()  == L'\'')) p.pop_back();
        if (!p.empty()) roots.push_back(p);
    }
    if (roots.empty()) { fprintf(stderr, "没有有效路径\n"); return 2; }

    // 输出是目录还是文件？
    DWORD attr = GetFileAttributesW(outArg.c_str());
    bool multiFile = (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY));
    // 以 .json 结尾且不存在 → 当成文件
    if (attr == INVALID_FILE_ATTRIBUTES) {
        std::wstring low = outArg;
        std::transform(low.begin(), low.end(), low.begin(), towlower);
        if (low.size() >= 5 && low.substr(low.size() - 5) == L".json") multiFile = false;
        else { CreateDirectoryW(outArg.c_str(), nullptr); multiFile = true; }
    }

    auto now = [] {
        SYSTEMTIME st; GetLocalTime(&st);
        char buf[32];
        sprintf(buf, "%04d-%02d-%02dT%02d:%02d:%02d",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        return std::string(buf);
    }();
    const std::string ts = now;

    // 单个根扫一次
    auto scanRoot = [&](const std::wstring& r, std::string& nodeJson, std::string& statsLine) -> bool {
        std::atomic<bool> cancel{false};
        ScanStats st;
        ULONGLONG lastPrint = 0;
        auto tick = [&](ULONGLONG items, ULONGLONG bytes, ULONGLONG) {
            ULONGLONG secs = GetTickCount64() / 1000;
            if (secs != lastPrint) {
                lastPrint = secs;
                fprintf(stderr, "  ... %llu 项 / %.1f MB\r", items, bytes / 1048576.0);
                fflush(stderr);
            }
        };
        ScanResult res = ScanTree(r, cancel, &st, tick, 8, false, false);
        if (!res.root) { statsLine = "扫描失败: " + W2U8(r); return false; }
        NodeToJson(res.root, nodeJson);
        char buf[256];
        sprintf(buf, "  OK %-12s 条目=%llu 字节=%llu 跳过=%llu",
                W2U8(r).c_str(), st.items, st.bytes, st.skipped);
        statsLine = buf;
        return true;
    };

    if (multiFile) {
        // 每个根一个文件：<目录>\<盘符或目录名>.json
        for (const auto& r : roots) {
            std::wstring name = r;
            // 去掉结尾反斜杠 → "C:\" → "C:"
            while (name.size() > 1 && (name.back() == L'\\' || name.back() == L'/')) name.pop_back();
            size_t pos = name.find_last_of(L"\\/");
            std::wstring base = (pos == std::wstring::npos) ? name : name.substr(pos + 1);
            if (base.empty()) base = L"root";
            // "C:" → 用盘符 C
            std::wstring fn = base;
            std::replace(fn.begin(), fn.end(), L':', L'_');
            std::wstring full = outArg + L"\\" + fn + L".json";

            fprintf(stderr, "[扫描] %s\n", W2U8(r).c_str());
            std::string nodeJson, line;
            if (!scanRoot(r, nodeJson, line)) { fprintf(stderr, "%s\n", line.c_str()); continue; }
            fprintf(stderr, "\r%s\n", line.c_str());

            std::string json = "{\"generatedAt\":\"" + ts + "\",\"root\":" + nodeJson + "}\n";
            FILE* f = _wfopen(full.c_str(), L"wb");
            if (!f) { fprintf(stderr, "写文件失败: %s\n", W2U8(full).c_str()); continue; }
            fwrite(json.data(), 1, json.size(), f);
            fclose(f);
            fprintf(stderr, "[写出] %s  (%.1f MB)\n", W2U8(full).c_str(), json.size() / 1048576.0);
        }
    } else {
        std::string json = "{\"generatedAt\":\"" + ts + "\",\"roots\":[";
        bool first = true;
        for (const auto& r : roots) {
            fprintf(stderr, "[扫描] %s\n", W2U8(r).c_str());
            std::string nodeJson, line;
            if (!scanRoot(r, nodeJson, line)) { fprintf(stderr, "%s\n", line.c_str()); continue; }
            fprintf(stderr, "\r%s\n", line.c_str());
            if (!first) json += ",";
            json += nodeJson;
            first = false;
        }
        json += "]}\n";
        FILE* f = _wfopen(outArg.c_str(), L"wb");
        if (!f) { fprintf(stderr, "写文件失败: %s\n", W2U8(outArg).c_str()); return 1; }
        fwrite(json.data(), 1, json.size(), f);
        fclose(f);
        fprintf(stderr, "[写出] %s  (%.1f MB)\n", W2U8(outArg).c_str(), json.size() / 1048576.0);
    }
    return 0;
}
