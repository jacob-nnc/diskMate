// scan_test：扫描器核心逻辑自检（控制台）
// 用法: scan_test [路径]   默认 C:\
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <clocale>
#include <cstdio>
#include <string>
#include <vector>

#include "scanner.h"
#include "utils.h"

int wmain(int argc, wchar_t** argv) {
    setlocale(LC_ALL, "");  // 控制台中文输出
    std::wstring root = (argc > 1) ? NormalizeRoot(argv[1]) : L"C:\\";
    if (!IsDirPath(root)) {
        wprintf(L"路径无效: %ls\n", root.c_str());
        return 2;
    }

    std::atomic<bool> cancel(false);
    ScanStats stats;
    auto t0 = std::chrono::steady_clock::now();
    ScanResult res = ScanTree(root, cancel, &stats, nullptr);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0)
                  .count();

    wprintf(L"根: %ls\n条目: %llu\n总字节: %llu (%ls)\n跳过: %llu\n耗时: %lld ms\n\n",
            root.c_str(), stats.items, stats.bytes, FormatSize(stats.bytes).c_str(),
            stats.skipped, ms);

    wprintf(L"前 5 大子项:\n");
    std::vector<ScanNode*> v(res.root->children.begin(), res.root->children.end());
    std::stable_sort(v.begin(), v.end(),
                     [](const ScanNode* a, const ScanNode* b) { return a->size > b->size; });
    for (int i = 0; i < 5 && i < (int)v.size(); i++) {
        wprintf(L"  %-40ls  %12ls  (%llu 文件)\n", v[i]->name.c_str(),
                FormatSize(v[i]->size).c_str(), v[i]->fileCount);
    }
    return 0;
}
