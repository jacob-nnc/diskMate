#pragma once
#include <windows.h>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// 扫描树节点：只存名称 + 聚合统计，完整路径按需通过 parent 链重建（省内存）
struct ScanNode {
    std::wstring name;
    ULONGLONG size = 0;       // 子树总大小（目录含后代，文件为自身）
    ULONGLONG fileCount = 0;  // 子树内文件数
    bool isDir = false;
    ScanNode* parent = nullptr;
    std::vector<ScanNode*> children;
};

// 扫描结果：arena 拥有全部节点内存，root 指向树根
struct ScanResult {
    ScanNode* root = nullptr;
    std::vector<std::unique_ptr<ScanNode>> arena;
};

struct ScanStats {
    ULONGLONG items = 0;     // 遇见的文件+目录数
    ULONGLONG bytes = 0;     // 文件字节总数
    ULONGLONG skipped = 0;   // 跳过数（无权限/重解析点等）
    bool cancelled = false;
};

// 迭代式并行目录扫描（不递归，避免深目录栈溢出）。
// cancel 为只读引用：置 true 后尽快停止，保留部分结果。
// progressTick 在后台线程调用（约每 80ms 一次），携带实时 (条目数, 字节数, 跳过数)。
// threads：工作线程数(1-16)；skipHidden：跳过隐藏/系统项；
// followReparse：跟随重解析点目录（基于目录身份自动防环）。
ScanResult ScanTree(const std::wstring& rootPath,
                    const std::atomic<bool>& cancel,
                    ScanStats* stats,
                    const std::function<void(ULONGLONG, ULONGLONG, ULONGLONG)>& progressTick,
                    int threads = 8,
                    bool skipHidden = false,
                    bool followReparse = false);

// 由节点重建完整路径
std::wstring NodeFullPath(const ScanNode* node);
