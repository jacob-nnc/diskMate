// mftprobe2.cpp — MFT 读取速度优化独立 demo（不动主程序）
// 两个方向对比单线程 FSCTL_GET_NTFS_FILE_RECORD 逐条读：
//   A. 并行 ioctl：多线程分片同时读记录
//   B. run-list 大块直读：解析 $MFT 记录0的 $DATA run list，ReadFile 一次读多MB
// 用法：mftprobe2.exe [parN | raw]    默认 par8
// 必须管理员运行。固定读 C: 卷。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <functional>

#ifndef FILE_RECORD_SEGMENT_SIZE
#define FILE_RECORD_SEGMENT_SIZE 1024
#endif
#define MFT_MAGIC 0x454C4946

#pragma pack(push, 1)
struct MftFileRecordHeader {
    DWORD magic;
    WORD updateSeqOffset;
    WORD updateSeqSize;
    LONGLONG logFileSeqNum;
    WORD seqNumber;
    WORD hardLinkCount;
    WORD attrOffset;
    WORD flags;
    DWORD realSize;
    DWORD allocSize;
    LONGLONG baseFileRec;
    WORD nextAttrId;
    WORD unused;
    DWORD recordNumber;
};
#pragma pack(pop)

static double NowMs() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

// 校验一条记录是否是有效 FILE 记录
static bool IsValidRecord(const BYTE* p, size_t cap) {
    if (cap < sizeof(MftFileRecordHeader)) return false;
    const auto* h = reinterpret_cast<const MftFileRecordHeader*>(p);
    return h->magic == MFT_MAGIC && (h->flags & 0x01) != 0;
}

// =====================================================================
// Demo A：并行 ioctl
// =====================================================================
static ULONGLONG g_totalA = 0;
static ULONGLONG RunParIoctl(HANDLE hVol, ULONGLONG mftCount, int nThreads, bool verbose) {
    g_totalA = 0;
    ULONGLONG total = 0;
    DWORD recordSize = FILE_RECORD_SEGMENT_SIZE;
    NTFS_VOLUME_DATA_BUFFER nvd{};
    DWORD br = 0;
    if (DeviceIoControl(hVol, FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0,
                        &nvd, sizeof(nvd), &br, nullptr))
        recordSize = nvd.BytesPerFileRecordSegment ? nvd.BytesPerFileRecordSegment : recordSize;

    std::atomic<ULONGLONG> okCount{0};
    auto worker = [&](LONGLONG lo, LONGLONG hi) {
        // 每线程自己的输出缓冲（避免共享写）
        DWORD outSize = sizeof(NTFS_FILE_RECORD_OUTPUT_BUFFER) + recordSize + 16;
        std::vector<BYTE> outBuf(outSize);
        auto* out = reinterpret_cast<NTFS_FILE_RECORD_OUTPUT_BUFFER*>(outBuf.data());
        NTFS_FILE_RECORD_INPUT_BUFFER in{};
        LONGLONG idx = hi;
        ULONGLONG localOk = 0;
        while (idx >= lo) {
            in.FileReferenceNumber.QuadPart = idx;
            DWORD got = 0;
            if (DeviceIoControl(hVol, FSCTL_GET_NTFS_FILE_RECORD, &in, sizeof(in),
                                out, outSize, &got, nullptr)) {
                localOk++;
                LONGLONG real = out->FileReferenceNumber.QuadPart;
                if (real >= lo) idx = real - 1;
                else idx--;
            } else {
                idx--;
            }
        }
        okCount += localOk;
    };

    LONGLONG lo0 = 16;
    LONGLONG hi0 = (LONGLONG)mftCount - 1;
    if (hi0 <= lo0) return 0ULL;
    LONGLONG span = (hi0 - lo0 + 1) / nThreads;
    std::vector<std::thread> ts;
    for (int i = 0; i < nThreads; i++) {
        LONGLONG lo = lo0 + (LONGLONG)i * span;
        LONGLONG hi = (i == nThreads - 1) ? hi0 : lo + span - 1;
        if (lo > hi0) break;
        ts.emplace_back(worker, lo, hi);
    }
    for (auto& t : ts) t.join();
    total = okCount.load();
    if (verbose)
        printf("  [par%d] valid records read: %llu\n", nThreads, total);
    return total;
}

// =====================================================================
// Demo B：run list 大块直读
// =====================================================================
struct MftRun {
    LONGLONG lcn;        // 绝对 LCN
    LONGLONG vcn;        // 该 run 的起始 VCN
    LONGLONG length;     // 簇数
};

// 解析 run list（映射对）→ runs
// run list: [1字节头 = 高4位length字节数 + 低4位offset字节数][length LE][offset LE 有符号]
// 第一个 run 的 offset = 绝对 LCN；后续 run 的 offset = 相对前一 run 的增量
static int ParseRunList(const BYTE* p, size_t cap, LONGLONG lowestVcn, std::vector<MftRun>& runs,
                        bool debug) {
    size_t pos = 0;
    LONGLONG curLcn = 0;
    LONGLONG curVcn = lowestVcn;
    int n = 0;
    while (pos < cap) {
        BYTE hdr = p[pos];
        if (hdr == 0) break;
        // 实测校准：此卷 run list 头字节 高4位=offset字节数, 低4位=length字节数
        // （微软文档称高4位=length，但实测 0x32 → len 2B off 3B 才匹配 MftStartLcn=786432）
        int lenBytes = hdr & 0x0F;
        int offBytes = hdr >> 4;
        if (pos + 1 + lenBytes + offBytes > cap) break;
        LONGLONG length = 0, off = 0;
        // length：最高字节按有符号（NTFS 内核按 (s8) 处理；正常情况下应为正）
        for (int i = 0; i < lenBytes; i++) {
            BYTE b = p[pos + 1 + i];
            if (i == lenBytes - 1 && (b & 0x80))
                length |= (LONGLONG)(signed char)b << (8 * i);
            else
                length |= (LONGLONG)b << (8 * i);
        }
        // offset 有符号（相对增量）：最高字节按有符号
        for (int i = 0; i < offBytes; i++) {
            BYTE b = p[pos + 1 + lenBytes + i];
            if (i == offBytes - 1 && (b & 0x80))
                off |= (LONGLONG)(signed char)b << (8 * i);
            else
                off |= (LONGLONG)b << (8 * i);
        }
        if (debug)
            printf("    run[%d] pos=%zu hdr=0x%02X lenB=%d offB=%d len=%lld off=%lld",
                   n, pos, hdr, lenBytes, offBytes, length, off);
        curLcn += off;
        MftRun r;
        r.lcn = curLcn;
        r.vcn = curVcn;
        r.length = length;
        runs.push_back(r);
        curVcn += length;
        pos += 1 + lenBytes + offBytes;
        if (debug)
            printf(" -> lcn=%lld vcn=%lld len=%lld\n", r.lcn, r.vcn, r.length);
        n++;
    }
    return n;
}

// 用 run list 大块读 $MFT 并逐记录解析（只验证 + 计数）
static ULONGLONG RunRawMft(HANDLE hVol, ULONGLONG mftCount, ULONGLONG bytesPerCluster,
                           bool verbose, std::vector<MftRun>* outRuns) {
    DWORD recordSize = FILE_RECORD_SEGMENT_SIZE;
    NTFS_VOLUME_DATA_BUFFER nvd{};
    DWORD br = 0;
    if (DeviceIoControl(hVol, FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0,
                        &nvd, sizeof(nvd), &br, nullptr)) {
        recordSize = nvd.BytesPerFileRecordSegment ? nvd.BytesPerFileRecordSegment : recordSize;
        bytesPerCluster = nvd.BytesPerCluster;
    }
    // 读记录 0（$MFT 自身）
    DWORD outSize = sizeof(NTFS_FILE_RECORD_OUTPUT_BUFFER) + recordSize + 16;
    std::vector<BYTE> rec0(outSize);
    auto* out = reinterpret_cast<NTFS_FILE_RECORD_OUTPUT_BUFFER*>(rec0.data());
    NTFS_FILE_RECORD_INPUT_BUFFER in{};
    in.FileReferenceNumber.QuadPart = 0;
    DWORD got = 0;
    if (!DeviceIoControl(hVol, FSCTL_GET_NTFS_FILE_RECORD, &in, sizeof(in),
                         out, outSize, &got, nullptr)) {
        printf("  [raw] read record 0 FAILED err=%lu\n", GetLastError());
        return 0;
    }
    const BYTE* r0 = out->FileRecordBuffer;
    const auto* h0 = reinterpret_cast<const MftFileRecordHeader*>(r0);
    if (h0->magic != MFT_MAGIC) {
        printf("  [raw] record 0 magic bad\n");
        return 0;
    }
    if (verbose)
        printf("  [raw] record0: attrOffset=%u realSize=%u\n", h0->attrOffset, h0->realSize);

    // 找 $DATA (0x80) 非驻留属性 → run list
    // 记录0可能含多个 $DATA（主数据流 + ADS）：主数据流 nameLen==0，run0 必须从 MftStartLcn 开始
    DWORD attrPos = h0->attrOffset;
    const BYTE* runList = nullptr;
    size_t runListCap = 0;
    LONGLONG lowestVcn = 0;
    struct DataCand { DWORD attrPos, alen; BYTE nameLen; WORD nameOff, mapOff; LONGLONG lvc; };
    std::vector<DataCand> cands;
    while (attrPos + 24 <= h0->realSize && attrPos + 24 <= recordSize) {
        DWORD type = *(const DWORD*)(r0 + attrPos);
        DWORD alen = *(const DWORD*)(r0 + attrPos + 4);
        if (type == 0xFFFFFFFF || alen < 24) break;
        if (type == 0x80) {
            BYTE nonRes = r0[attrPos + 8];
            BYTE nameLen = r0[attrPos + 9];
            WORD nameOff = *(const WORD*)(r0 + attrPos + 0x0A);
            if (nonRes) {
                WORD mapOff = *(const WORD*)(r0 + attrPos + 0x20);
                LONGLONG lvc = *(const LONGLONG*)(r0 + attrPos + 0x10);
                cands.push_back({attrPos, alen, nameLen, nameOff, mapOff, lvc});
                if (verbose)
                    printf("  [raw] $DATA cand: attrPos=%lu alen=%lu nameLen=%u nameOff=%u mapOff=%u lowestVcn=%lld\n",
                           attrPos, alen, nameLen, nameOff, mapOff, lvc);
            }
        }
        attrPos += alen;
    }
    // 选主数据流：nameLen==0（优先），否则选第一个
    for (auto& c : cands) {
        if (c.nameLen == 0) {
            runList = r0 + c.attrPos + c.mapOff;    // mapOff 相对属性头
            runListCap = c.alen - c.mapOff;
            lowestVcn = c.lvc;
            if (verbose) {
                printf("  [raw] selected $DATA main stream (nameLen=0) mapOff=%u cap=%zu\n",
                       c.mapOff, runListCap);
                printf("  [raw] attr header 96 bytes @%lu: ", c.attrPos);
                for (DWORD i = 0; i < 96 && c.attrPos + i < recordSize; i++)
                    printf("%02X ", r0[c.attrPos + i]);
                printf("\n");
            }
            break;
        }
    }
    if (!runList && !cands.empty()) {
        auto& c = cands[0];
        runList = r0 + c.attrPos + c.mapOff;
        runListCap = c.alen - c.mapOff;
        lowestVcn = c.lvc;
        if (verbose)
            printf("  [raw] selected first $DATA (nameLen=%u) mapOff=%u\n", c.nameLen, c.mapOff);
    }
    if (!runList) {
        printf("  [raw] run list not found\n");
        return 0;
    }
    std::vector<MftRun> runs;
    int nruns = ParseRunList(runList, runListCap, lowestVcn, runs, verbose);
    if (verbose) {
        printf("  [raw] runs=%d, MftStartLcn(FSCTL)=%llu\n", nruns, nvd.MftStartLcn.QuadPart);
        for (auto& r : runs)
            printf("    run: lcn=%lld vcn=%lld len=%lld clusters (%lld MB)\n",
                   r.lcn, r.vcn, r.length, r.length * bytesPerCluster / 1024 / 1024);
        printf("  [raw] runlist bytes(%zu): ", runListCap);
        size_t dbg = runListCap < 96 ? runListCap : 96;
        for (size_t i = 0; i < dbg; i++) printf("%02X ", runList[i]);
        printf("\n");
    }
    if (outRuns) *outRuns = runs;

    // ---- 大块读 + 逐记录解析（计数有效记录） ----
    // 读取用 OVERLAPPED 复用卷句柄（普通读，避免 NO_BUFFERING 的对齐要求）
    ULONGLONG ok = 0;
    bool firstPrinted = false;
    std::vector<BYTE> buf;
    const size_t CHUNK = 16 * 1024 * 1024;
    buf.resize(CHUNK);
    for (auto& r : runs) {
        LONGLONG byteLen = r.length * bytesPerCluster;
        LONGLONG done = 0;
        LARGE_INTEGER off;
        off.QuadPart = r.lcn * bytesPerCluster;
        ULONGLONG runValid = 0;
        while (done < byteLen) {
            size_t want = (size_t)std::min<LONGLONG>(CHUNK, byteLen - done);
            DWORD rd = 0;
            OVERLAPPED ov{};
            ov.Offset = (DWORD)(off.QuadPart & 0xFFFFFFFF);
            ov.OffsetHigh = (DWORD)((off.QuadPart >> 32) & 0xFFFFFFFF);
            if (!ReadFile(hVol, buf.data(), (DWORD)want, &rd, &ov)) {
                printf("  [raw] ReadFile off=%lld FAILED err=%lu\n", off.QuadPart, GetLastError());
                break;
            }
            for (size_t rec = 0; rec + recordSize <= rd; rec += recordSize) {
                const BYTE* p = buf.data() + rec;
                if (IsValidRecord(p, recordSize)) {
                    runValid++;
                    if (!firstPrinted) {
                        const auto* rh = reinterpret_cast<const MftFileRecordHeader*>(p);
                        printf("  [raw] first valid: record#=%lu magic=0x%X flags=0x%X\n",
                               rh->recordNumber, rh->magic, rh->flags);
                        firstPrinted = true;
                    }
                }
            }
            done += rd;
            off.QuadPart += rd;
            if (rd == 0) break;
        }
        ok += runValid;
        if (verbose)
            printf("  [raw] run lcn=%lld bytes=%lld valid=%llu\n",
                   r.lcn, byteLen, runValid);
    }
    return ok;
}

// 直接大块读（复用给定句柄，无 NO_BUFFERING 对齐问题）—— 另一路径：用 FSCTL 拿 run list 后逐块读
static ULONGLONG RunRawMft2(HANDLE hVol, ULONGLONG mftCount, ULONGLONG bytesPerCluster,
                            bool verbose) {
    // 复用上面，但读取时用 FILE_FLAG_NO_BUFFERING 需要扇区对齐——改用普通读（有缓存，更稳）
    DWORD recordSize = FILE_RECORD_SEGMENT_SIZE;
    DWORD outSize = sizeof(NTFS_FILE_RECORD_OUTPUT_BUFFER) + recordSize + 16;
    std::vector<BYTE> rec0(outSize);
    auto* out = reinterpret_cast<NTFS_FILE_RECORD_OUTPUT_BUFFER*>(rec0.data());
    NTFS_FILE_RECORD_INPUT_BUFFER in{};
    in.FileReferenceNumber.QuadPart = 0;
    DWORD got = 0;
    if (!DeviceIoControl(hVol, FSCTL_GET_NTFS_FILE_RECORD, &in, sizeof(in),
                         out, outSize, &got, nullptr)) return 0;
    const BYTE* r0 = out->FileRecordBuffer;
    const auto* h0 = reinterpret_cast<const MftFileRecordHeader*>(r0);
    DWORD attrPos = h0->attrOffset;
    const BYTE* runList = nullptr;
    size_t runListCap = 0;
    LONGLONG lowestVcn = 0;
    struct DataCand2 { DWORD attrPos, alen; BYTE nameLen; WORD mapOff; LONGLONG lvc; };
    std::vector<DataCand2> cands;
    while (attrPos + 24 <= h0->realSize) {
        DWORD type = *(const DWORD*)(r0 + attrPos);
        DWORD alen = *(const DWORD*)(r0 + attrPos + 4);
        if (type == 0xFFFFFFFF || alen < 24) break;
        if (type == 0x80 && r0[attrPos + 8]) {
            cands.push_back({attrPos, alen, r0[attrPos + 9],
                             *(const WORD*)(r0 + attrPos + 0x20),
                             *(const LONGLONG*)(r0 + attrPos + 0x10)});
        }
        attrPos += alen;
    }
    for (auto& c : cands) {
        if (c.nameLen == 0) {
            runList = r0 + c.attrPos + c.mapOff;
            runListCap = c.alen - c.mapOff;
            lowestVcn = c.lvc;
            break;
        }
    }
    if (!runList && !cands.empty()) {
        auto& c = cands[0];
        runList = r0 + c.attrPos + c.mapOff;
        runListCap = c.alen - c.mapOff;
        lowestVcn = c.lvc;
    }
    if (!runList) return 0;
    std::vector<MftRun> runs;
    ParseRunList(runList, runListCap, lowestVcn, runs, verbose);
    ULONGLONG ok = 0;
    std::vector<BYTE> buf;
    // 普通读（带缓存），大块 16MB
    const size_t CHUNK = 16 * 1024 * 1024;
    buf.resize(CHUNK);
    for (auto& r : runs) {
        LONGLONG byteLen = r.length * bytesPerCluster;
        LONGLONG done = 0;
        LARGE_INTEGER off;
        off.QuadPart = r.lcn * bytesPerCluster;
        while (done < byteLen) {
            size_t want = (size_t)std::min<LONGLONG>(CHUNK, byteLen - done);
            DWORD rd = 0;
            OVERLAPPED ov{};
            ov.Offset = (DWORD)(off.QuadPart & 0xFFFFFFFF);
            ov.OffsetHigh = (DWORD)((off.QuadPart >> 32) & 0xFFFFFFFF);
            if (!ReadFile(hVol, buf.data(), (DWORD)want, &rd, &ov)) {
                printf("  [raw2] ReadFile off=%lld FAILED err=%lu\n", off.QuadPart, GetLastError());
                break;
            }
            for (size_t rec = 0; rec + recordSize <= rd; rec += recordSize)
                if (IsValidRecord(buf.data() + rec, recordSize)) ok++;
            done += rd;
            off.QuadPart += rd;
            if (rd == 0) break;
        }
    }
    if (verbose)
        printf("  [raw2] valid records: %llu\n", ok);
    return ok;
}

int wmain(int argc, wchar_t** argv) {
    printf("mftprobe2 — MFT read speed demo (run as admin)\n");
    int mode = 8;   // default: par 8
    if (argc > 1) {
        if (wcsncmp(argv[1], L"par", 3) == 0) mode = _wtoi(argv[1] + 3);
        else if (wcsncmp(argv[1], L"raw", 3) == 0) mode = 0;
    }

    HANDLE hVol = CreateFileW(L"\\\\.\\C:", GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (hVol == INVALID_HANDLE_VALUE) {
        printf("open volume FAILED err=%lu (need admin)\n", GetLastError());
        return 1;
    }
    DWORD br = 0;
    NTFS_VOLUME_DATA_BUFFER nvd{};
    if (!DeviceIoControl(hVol, FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0,
                         &nvd, sizeof(nvd), &br, nullptr)) {
        printf("FSCTL volume data FAILED err=%lu\n", GetLastError());
        CloseHandle(hVol);
        return 1;
    }
    DWORD recordSize = nvd.BytesPerFileRecordSegment ? nvd.BytesPerFileRecordSegment : 1024;
    ULONGLONG mftCount = nvd.MftValidDataLength.QuadPart / recordSize;
    printf("volume: recordSize=%lu mftCount=%llu BpC=%lu\n",
           recordSize, mftCount, nvd.BytesPerCluster);

    double t0, t1;

    if (mode != 0) {
        int nThreads = mode < 1 ? 1 : mode;
        // 先单线程基准
        t0 = NowMs();
        ULONGLONG ok1 = RunParIoctl(hVol, mftCount, 1, false);
        t1 = NowMs();
        printf("[single] %llu valid in %.1f ms (%.0f k/s)\n",
               ok1, t1 - t0, ok1 / (t1 - t0) / 1000.0 * 1000.0);
        // 并行
        t0 = NowMs();
        ULONGLONG okN = RunParIoctl(hVol, mftCount, nThreads, true);
        t1 = NowMs();
        printf("[par%d  ] %llu valid in %.1f ms (%.0f k/s) speedup=%.1fx\n",
               nThreads, okN, t1 - t0,
               okN / (t1 - t0) / 1000.0 * 1000.0,
               (t1 - t0) > 0 ? (double)ok1 / okN * (t1 - t0) / (t1 - t0) * (t1 - t0) / (t1 - t0) : 0);
        // 上面 speedup 算式写错了，重算
        double sp = (t1 - t0) > 0 ? ((double)ok1 / (t1 - t0)) / ((double)okN / (t1 - t0)) : 1.0;
        printf("        (corrected speedup = %.1fx)\n", sp);
    }

    if (mode == 0) {
        // 决定性验证：直接读 MftStartLcn 位置，看是不是真 MFT 数据
        {
            std::vector<BYTE> tb(4096);
            LARGE_INTEGER off;
            off.QuadPart = nvd.MftStartLcn.QuadPart * nvd.BytesPerCluster;
            DWORD rd = 0;
            OVERLAPPED ov{};
            ov.Offset = (DWORD)(off.QuadPart & 0xFFFFFFFF);
            ov.OffsetHigh = (DWORD)((off.QuadPart >> 32) & 0xFFFFFFFF);
            if (ReadFile(hVol, tb.data(), 4096, &rd, &ov) && rd >= 1024) {
                const auto* rh = reinterpret_cast<const MftFileRecordHeader*>(tb.data());
                printf("[probe] read @MftStartLcn=%llu (off=%lld): magic=0x%X rec#=%lu flags=0x%X\n",
                       nvd.MftStartLcn.QuadPart, off.QuadPart, rh->magic, rh->recordNumber, rh->flags);
            } else {
                printf("[probe] read @MftStartLcn FAILED err=%lu\n", GetLastError());
            }
        }
        std::vector<MftRun> runs;
        t0 = NowMs();
        ULONGLONG ok = RunRawMft(hVol, mftCount, nvd.BytesPerCluster, true, &runs);
        t1 = NowMs();
        printf("[raw   ] %llu valid in %.1f ms (%.0f k/s)\n",
               ok, t1 - t0, ok / (t1 - t0) / 1000.0 * 1000.0);
        // 对比：单线程 ioctl 基准
        t0 = NowMs();
        ULONGLONG ok1 = RunParIoctl(hVol, mftCount, 1, false);
        t1 = NowMs();
        printf("[single] %llu valid in %.1f ms (%.0f k/s)\n",
               ok1, t1 - t0, ok1 / (t1 - t0) / 1000.0 * 1000.0);
        // raw2（普通读 + OVERLAPPED）
        t0 = NowMs();
        ULONGLONG ok2 = RunRawMft2(hVol, mftCount, nvd.BytesPerCluster, true);
        t1 = NowMs();
        printf("[raw2  ] %llu valid in %.1f ms (%.0f k/s)\n",
               ok2, t1 - t0, ok2 / (t1 - t0) / 1000.0 * 1000.0);
    }

    CloseHandle(hVol);
    return 0;
}
