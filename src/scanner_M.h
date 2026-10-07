#pragma once
#include "scanner.h"   // 复用 ScanNode/ScanStats/ScanResult/NodeFullPath（唯一实现，避免重定义）

// ============================================================================
// MFT 直读扫描器（NTFS 卷，需管理员权限）
//   直接打开卷设备 "\\.\C:" 解析 MFT，不递归目录，全盘扫描速度远超目录遍历。
//   接口与 ScanTree 完全对齐，可无缝替换（上层代码无需修改）。
//   打开卷失败 / 非 NTFS / 无权限时返回 root==nullptr 的空结果，
//   调用方应据此降级到普通目录遍历 ScanTree。
//   已知限制：单线程顺序读 MFT；无硬链接去重；逻辑大小而非占用空间；无超时。
// ============================================================================
ScanResult ScanTreeMFT(const std::wstring& volumePath,
                       const std::atomic<bool>& cancel,
                       ScanStats* stats,
                       const std::function<void(ULONGLONG, ULONGLONG, ULONGLONG)>& progressTick,
                       int threads,
                       bool skipHidden,
                       bool followReparse);
