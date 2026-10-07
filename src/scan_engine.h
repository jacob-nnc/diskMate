#pragma once
#include "scanner.h"
#include "scanner_M.h"
#include "settings.h"
#include <atomic>
#include <functional>
#include <memory>
#include <string>

// ============================================================================
// 扫描引擎：进程内公共 API，两个前端（WebView2 版 / 原生 C++ 版）共用。
//   · 统一线程编排：MFT 直读优先（请求 MFT 时），失败自动降级普通遍历
//   · 取消 / 进度（内部 80ms 节流）/ 结果树所有权
//   · 回调在【扫描线程】触发；调用方自行决定如何投递到 UI 线程
// ============================================================================

enum class ScanMode { Walk, Mft };   // 请求模式；MFT 失败时实际以 Walk 完成

struct ScanDoneInfo {
    std::unique_ptr<ScanResult> tree;  // 结果树（move 语义，所有权转移给调用方）
    ScanStats stats;
    ScanMode mode;         // 实际完成模式（可能降级）
    std::wstring root;
};

class ScanEngine {
public:
    using ProgressFn = std::function<void(ULONGLONG items, ULONGLONG bytes, ULONGLONG skipped)>;
    using DoneFn = std::function<void(ScanDoneInfo&&)>;
    using FailFn = std::function<void(const std::wstring& reason)>;

    struct Job;   // 内部（线程参数）

    void SetCallbacks(ProgressFn p, DoneFn d, FailFn f) {
        m_prog = std::move(p); m_done = std::move(d); m_fail = std::move(f);
    }

    // 启动扫描。若正在扫，先取消旧任务并等其退出。返回是否已启动。
    bool Start(const std::wstring& root, ScanMode reqMode, const AppSettings& cfg);

    void Cancel();                 // 置取消标志，扫描线程尽快退出
    bool Running() const { return m_running.load(); }
    ScanMode RequestedMode() const { return m_reqMode; }
    ScanStats LastStats() const { return m_stats; }
    ScanMode ActualMode() const { return m_actualMode; }
    bool LastCancelled() const { return m_cancelled; }

private:
    static void ThreadProc(std::unique_ptr<Job> job);
    void Run(const std::wstring& root, ScanMode reqMode, const AppSettings& cfg);

    std::shared_ptr<std::atomic<bool>> m_cancel;
    std::atomic<bool> m_running{ false };
    ScanMode m_reqMode = ScanMode::Walk;
    ScanMode m_actualMode = ScanMode::Walk;
    bool m_cancelled = false;
    ScanStats m_stats;
    ProgressFn m_prog;
    DoneFn m_done;
    FailFn m_fail;
};

// 判断当前进程是否管理员（提权逻辑复用）
bool IsProcessElevated();
