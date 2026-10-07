#include "scanner.h"

#include <algorithm>
#include <condition_variable>
#include <cwchar>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_set>

static bool IsDotEntry(const wchar_t* s) {
    return wcscmp(s, L".") == 0 || wcscmp(s, L"..") == 0;
}


// 目录身份（卷序列号 + 文件索引），用于重解析点防环
static bool GetDirId(const std::wstring& path, ULONGLONG* out) {
    HANDLE h = CreateFileW(path.c_str(), 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info;
    bool ok = GetFileInformationByHandle(h, &info);
    CloseHandle(h);
    if (!ok) return false;
    *out = ((ULONGLONG)info.dwVolumeSerialNumber << 32) |
           (((ULONGLONG)info.nFileIndexHigh << 32) | info.nFileIndexLow);
    return true;
}

// 并行目录扫描：
//   - 多个工作线程共用一个任务队列（目录级并行，枚举在锁外进行）
//   - FindFirstFileExW + FIND_FIRST_EX_LARGE_FETCH 让 NTFS 批量预取目录项
//   - 节点在短临界区内批量分配，挂接/聚合/入队一次完成
//   - 进度回调由工作线程携带实时计数调用（时间节流，不阻塞）
ScanResult ScanTree(const std::wstring& rootPath,
                    const std::atomic<bool>& cancel,
                    ScanStats* stats,
                    const std::function<void(ULONGLONG, ULONGLONG, ULONGLONG)>& progressTick,
                    int threads,
                    bool skipHidden,
                    bool followReparse) {
    struct Shared {
        std::mutex mtx;
        std::condition_variable cv;
        std::deque<std::pair<ScanNode*, std::wstring>> queue;
        std::vector<std::unique_ptr<ScanNode>> arena;
        std::unordered_set<ULONGLONG> visited;  // 重解析点防环
        std::atomic<ULONGLONG> items{0};
        std::atomic<ULONGLONG> bytes{0};
        std::atomic<ULONGLONG> skipped{0};
        std::atomic<ULONGLONG> lastTick{0};  // 进度节流
        std::atomic<int> remaining{0};
    } sh;

    std::wstring root = rootPath;
    while (root.size() > 3 && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
    if (root.empty()) root = L"C:\\";

    ScanNode* rootNode = nullptr;
    {
        sh.arena.push_back(std::make_unique<ScanNode>());
        rootNode = sh.arena.back().get();
        rootNode->name = root;
        rootNode->isDir = true;
        rootNode->parent = nullptr;
    }
    sh.queue.push_back({ rootNode, root });
    sh.remaining.store(1);

    struct Entry {
        std::wstring name;
        std::wstring subPath;  // 仅目录有
        ULONGLONG size;
        bool isDir;
    };

    unsigned nThreads = (unsigned)threads;
    if (nThreads < 1) nThreads = 1;
    if (nThreads > 16) nThreads = 16;

    auto worker = [&]() {
        for (;;) {
            std::pair<ScanNode*, std::wstring> w;
            {
                std::unique_lock<std::mutex> lk(sh.mtx);
                sh.cv.wait(lk, [&] {
                    return !sh.queue.empty() || sh.remaining.load() == 0;
                });
                if (sh.queue.empty() && sh.remaining.load() == 0) return;
                w = std::move(sh.queue.front());
                sh.queue.pop_front();
            }

            if (cancel.load()) {
                std::lock_guard<std::mutex> lk(sh.mtx);
                sh.remaining--;
                sh.cv.notify_all();
                continue;
            }

            // ---- 枚举当前目录（锁外，最耗时部分） ----
            std::wstring pattern = w.second;
            if (pattern.back() != L'\\' && pattern.back() != L'/') pattern.push_back(L'\\');
            pattern += L'*';

            std::vector<Entry> entries;
            ULONGLONG localBytes = 0, localSkipped = 0;
            {
                WIN32_FIND_DATAW fd;
                HANDLE hFind = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd,
                                                FindExSearchNameMatch, nullptr,
                                                FIND_FIRST_EX_LARGE_FETCH);
                if (hFind == INVALID_HANDLE_VALUE) {
                    std::lock_guard<std::mutex> lk(sh.mtx);
                    sh.skipped++;
                    sh.remaining--;
                    sh.cv.notify_all();
                    continue;
                }
                do {
                    if (cancel.load()) break;
                    if (IsDotEntry(fd.cFileName)) continue;
                    const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                    if (skipHidden &&
                        (fd.dwFileAttributes &
                         (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0) {
                        localSkipped++;
                        continue;
                    }
                    const bool isReparse =
                        (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
                    std::wstring childPath;
                    if (isDir) {
                        childPath = w.second;
                        if (childPath.back() != L'\\' && childPath.back() != L'/')
                            childPath.push_back(L'\\');
                        childPath += fd.cFileName;
                    }
                    if (isDir && isReparse) {
                        if (!followReparse) {
                            localSkipped++;
                            continue;
                        }
                        ULONGLONG id;
                        if (!GetDirId(childPath, &id)) {
                            localSkipped++;
                            continue;
                        }
                        bool fresh;
                        {
                            std::lock_guard<std::mutex> lk(sh.mtx);
                            fresh = sh.visited.insert(id).second;
                        }
                        if (!fresh) {
                            localSkipped++;  // 防环：同一目录只扫一次
                            continue;
                        }
                    }
                    Entry e;
                    e.name = fd.cFileName;
                    e.isDir = isDir;
                    e.size = isDir ? 0 : ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                    if (isDir) e.subPath = std::move(childPath);
                    localBytes += e.size;
                    entries.push_back(std::move(e));
                } while (FindNextFileW(hFind, &fd));
                FindClose(hFind);
            }

            // ---- 批量分配节点（短临界区） ----
            std::vector<ScanNode*> nodes;
            if (!entries.empty()) {
                std::lock_guard<std::mutex> lk(sh.mtx);
                nodes.reserve(entries.size());
                for (size_t i = 0; i < entries.size(); i++) {
                    sh.arena.push_back(std::make_unique<ScanNode>());
                    nodes.push_back(sh.arena.back().get());
                }
            }

            // ---- 填充（锁外） ----
            for (size_t i = 0; i < nodes.size(); i++) {
                ScanNode* c = nodes[i];
                c->name = entries[i].name;
                c->isDir = entries[i].isDir;
                c->size = entries[i].size;
                if (!c->isDir) c->fileCount = 1;
                c->parent = w.first;
            }

            // ---- 挂接 + 祖先聚合 + 子目录入队（短临界区） ----
            {
                std::lock_guard<std::mutex> lk(sh.mtx);
                for (size_t i = 0; i < nodes.size(); i++) {
                    ScanNode* c = nodes[i];
                    w.first->children.push_back(c);
                    for (ScanNode* p = w.first; p; p = p->parent) {
                        p->size += c->size;
                        if (!c->isDir) p->fileCount += 1;
                    }
                    if (c->isDir) {
                        sh.queue.push_back({ c, entries[i].subPath });
                        sh.remaining++;
                    }
                }
                sh.items += (ULONGLONG)nodes.size();
                sh.bytes += localBytes;
                sh.skipped += localSkipped;
                sh.remaining--;
            }
            sh.cv.notify_all();

            // ---- 实时进度（约 80ms 节流，携带真实计数） ----
            if (progressTick) {
                ULONGLONG now = GetTickCount64();
                if (now - sh.lastTick.load() >= 80) {
                    sh.lastTick.store(now);
                    progressTick(sh.items.load(), sh.bytes.load(), sh.skipped.load());
                }
            }
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(nThreads);
    for (unsigned i = 0; i < nThreads; i++) workers.emplace_back(worker);
    for (auto& t : workers) t.join();

    stats->items = sh.items.load();
    stats->bytes = sh.bytes.load();
    stats->skipped = sh.skipped.load();
    stats->cancelled = cancel.load();

    ScanResult result;
    result.root = rootNode;
    result.arena = std::move(sh.arena);
    return result;
}

std::wstring NodeFullPath(const ScanNode* node) {
    std::vector<std::wstring> parts;
    std::unordered_set<const ScanNode*> seen;   // 防环：MFT 树可能有父子互引
    for (const ScanNode* n = node; n && seen.insert(n).second; n = n->parent)
        parts.push_back(n->name);
    std::wstring res;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (res.empty()) {
            res = *it;
            continue;
        }
        if (res.back() != L'\\' && res.back() != L'/' && it->front() != L'\\' && it->front() != L'/')
            res.push_back(L'\\');
        res += *it;
    }
    return res;
}
