// sizelist：递归文件大小嵌套列表（方括号数组格式），全量输出
// 每项: [字节数]（文件） 或 [字节数, [子项数组]]（目录）；仅大小，无文件名
// 弹窗显示全部内容，同时把完整列表写入剪贴板（CF_UNICODETEXT）
// 用法: sizelist [根路径]   默认根: C:\Users\Jacob
#include <windows.h>
#include <conio.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <clocale>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "scanner.h"
#include "utils.h"

static FILE* g_fp = nullptr;

static void WriteOut(const std::wstring& line) {
    if (line.empty()) return;
    int n = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), nullptr, 0,
                                nullptr, nullptr);
    std::string u8(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), &u8[0], n, nullptr,
                        nullptr);
    fwrite(u8.data(), 1, u8.size(), stdout);
    fflush(stdout);
    if (g_fp) fwrite(u8.data(), 1, u8.size(), g_fp);
}

// 完整列表写入系统剪贴板（Unicode 文本）
static void SetClipboard(const std::wstring& text) {
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
        wchar_t* p = (wchar_t*)GlobalLock(h);
        if (p) {
            memcpy(p, text.c_str(), text.size() * sizeof(wchar_t));
            p[text.size()] = L'\0';
            GlobalUnlock(h);
            if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
        } else {
            GlobalFree(h);
        }
    }
    CloseClipboard();
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    setlocale(LC_ALL, "");

    std::wstring root = L"C:\\Users\\Jacob";
    for (int i = 1; i < argc; i++)
        if (argv[i][0] != L'-') root = NormalizeRoot(argv[i]);
    if (!IsDirPath(root)) {
        WriteOut(L"路径无效: " + root + L"\n");
        _getwch();
        return 2;
    }

    WriteOut(L"正在扫描 " + root + L" ...\n");

    std::atomic<bool> cancel(false);
    ScanStats stats;
    auto t0 = std::chrono::steady_clock::now();
    ScanResult res = ScanTree(root, cancel, &stats, nullptr);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0)
                  .count();
    if (!res.root) {
        WriteOut(L"扫描失败\n");
        _getwch();
        return 3;
    }

    // 文件写到 exe 同目录（绝对路径）
    wchar_t exePath[MAX_PATH] = { 0 };
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring txtPath = exePath;
    size_t slash = txtPath.find_last_of(L'\\');
    if (slash != std::wstring::npos) txtPath.resize(slash + 1);
    txtPath += L"sizelist.txt";
    g_fp = _wfopen(txtPath.c_str(), L"wb");
    if (g_fp) {
        const char bom[3] = { (char)0xEF, (char)0xBB, (char)0xBF };
        fwrite(bom, 1, 3, g_fp);
    }

    // 构建嵌套数组：全量，每层按大小降序，无过滤、无限制
    std::wstring sb;
    sb.reserve((size_t)(stats.items * 40));
    std::function<void(const ScanNode*, int)> build;
    build = [&](const ScanNode* node, int depth) {
        std::wstring ind(depth * 2, L' ');
        std::vector<ScanNode*> kids;
        for (auto* c : node->children)
            if (c && c->size > 0) kids.push_back(c);
        std::stable_sort(kids.begin(), kids.end(),
                         [](const ScanNode* a, const ScanNode* b) { return a->size > b->size; });
        wchar_t buf[64];
        swprintf(buf, 64, L"%ls[%llu", ind.c_str(), node->size);
        sb += buf;
        if (kids.empty()) {
            sb += L"]";
            return;
        }
        sb += L", \n";
        for (size_t i = 0; i < kids.size(); i++) {
            build(kids[i], depth + 1);
            sb += (i + 1 < kids.size()) ? L",\n" : L"\n";
        }
        sb += ind + L"]";
    };

    sb += L"[\n";
    std::vector<ScanNode*> roots;
    for (auto* c : res.root->children)
        if (c && c->size > 0) roots.push_back(c);
    std::stable_sort(roots.begin(), roots.end(),
                     [](const ScanNode* a, const ScanNode* b) { return a->size > b->size; });
    for (size_t i = 0; i < roots.size(); i++) {
        build(roots[i], 1);
        sb += (i + 1 < roots.size()) ? L",\n" : L"\n";
    }
    sb += L"]\n";

    // 写入剪贴板
    SetClipboard(sb);

    // 弹窗显示
    wchar_t hdr[256];
    swprintf(hdr, 256, L"%ls  (%llu 项, %ls, %lld ms)\n\n", root.c_str(), stats.items,
             FormatSize(stats.bytes).c_str(), ms);
    WriteOut(hdr);
    WriteOut(sb);
    if (g_fp) fclose(g_fp);

    wchar_t tail[256];
    swprintf(tail, 256, L"\n(已写入剪贴板, 已保存 %ls)\n按任意键关闭...\n", txtPath.c_str());
    WriteOut(tail);
    _getwch();
    return 0;
}
