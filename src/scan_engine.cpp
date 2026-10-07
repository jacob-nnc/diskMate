#include "scan_engine.h"
#include "utils.h"
#include <windows.h>
#include <thread>

// ---------- 提权判断 ----------
bool IsProcessElevated() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) return false;
    TOKEN_ELEVATION te{};
    DWORD len = 0;
    BOOL ok = GetTokenInformation(hToken, TokenElevation, &te, sizeof(te), &len);
    CloseHandle(hToken);
    return ok && te.TokenIsElevated != 0;
}

// ---------- 扫描线程 ----------
struct ScanEngine::Job {
    ScanEngine* self;
    std::wstring root;
    ScanMode mode;
    AppSettings cfg;
};

void ScanEngine::ThreadProc(std::unique_ptr<Job> job) {
    job->self->Run(job->root, job->mode, job->cfg);
}

bool ScanEngine::Start(const std::wstring& root, ScanMode reqMode, const AppSettings& cfg) {
    if (m_running.load()) return false;   // 调用方负责提示"已有扫描"
    m_reqMode = reqMode;
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    m_running.store(true);
    m_cancelled = false;
    auto* raw = new Job{ this, root, reqMode, cfg };
    std::thread th(ThreadProc, std::unique_ptr<Job>(raw));
    th.detach();
    return true;
}

void ScanEngine::Cancel() {
    if (m_cancel) m_cancel->store(true);
}

void ScanEngine::Run(const std::wstring& root, ScanMode reqMode, const AppSettings& cfg) {
    ScanStats stats;
    // 进度节流（~80ms；回调在扫描线程触发，调用方自行投递到 UI 线程）
    std::atomic<ULONGLONG> lastTick{ 0 };
    auto progress = [&](ULONGLONG items, ULONGLONG bytes, ULONGLONG skipped) {
        ULONGLONG now = GetTickCount64();
        if (now - lastTick.load() >= 80) {
            lastTick.store(now);
            if (m_prog) m_prog(items, bytes, skipped);
        }
    };

    ScanResult res;
    ScanMode actual = ScanMode::Walk;
    bool cancelled = m_cancel && m_cancel->load();

    // MFT 直读：请求 MFT 时才尝试；打开/解析失败自动降级普通遍历
    if (reqMode == ScanMode::Mft && !cancelled) {
        std::wstring vol;
        if (root.size() >= 2 && root[1] == L':')
            vol = L"\\\\.\\" + root.substr(0, 1) + L":";
        if (!vol.empty()) {
            ScanStats mftStats;
            ScanResult mr = ScanTreeMFT(vol, *m_cancel, &mftStats, progress,
                                        cfg.threads, cfg.skipHidden, cfg.followReparse);
            if (mr.root) {
                res = std::move(mr);
                stats = mftStats;
                actual = ScanMode::Mft;
            }
            // 失败：静默降级（调用方通过 ActualMode 得知）
        }
    }
    cancelled = m_cancel && m_cancel->load();
    if (!cancelled && res.root == nullptr) {
        res = ScanTree(root, *m_cancel, &stats, progress,
                       cfg.threads, cfg.skipHidden, cfg.followReparse);
        actual = ScanMode::Walk;
    }

    m_actualMode = actual;
    m_stats = stats;
    m_cancelled = m_cancel && m_cancel->load();
    m_running.store(false);
    if (!m_cancelled && res.root) {
        if (m_done)
            m_done(ScanDoneInfo{ std::make_unique<ScanResult>(std::move(res)), stats, actual, root });
    } else {
        if (m_fail) m_fail(m_cancelled ? L"已取消" : L"扫描无结果");
    }
}
