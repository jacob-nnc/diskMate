#include "scanner_M.h"
#include <windows.h>
#include <winioctl.h>
#include <algorithm>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cwchar>
#include <cstdio>
#include <share.h>

// NTFS MFT 常量定义
#ifndef FILE_RECORD_SEGMENT_SIZE
#define FILE_RECORD_SEGMENT_SIZE 1024
#endif
#define MFT_MAGIC 0x454C4946 // "FILE"

// MinGW winioctl.h lacks these MSVC-only volume bitmap definitions; add guards
#ifndef IOCTL_VOLUME_GET_BITMAP_INFORMATION
#ifndef IOCTL_VOLUME_BASE
#define IOCTL_VOLUME_BASE ((DWORD)'V')
#endif
#define IOCTL_VOLUME_GET_BITMAP_INFORMATION CTL_CODE(IOCTL_VOLUME_BASE, 1, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef VOLUME_GET_BITMAP_INFORMATION
typedef struct {
    LARGE_INTEGER StartingLcn;
    LARGE_INTEGER BitmapSize;
} VOLUME_GET_BITMAP_INFORMATION;
#endif

#pragma pack(push, 1)
// 简化MFT记录头，仅需要我们用到的字段
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
    LONGLONG baseFileRec; // base mft ref
    WORD nextAttrId;
    WORD unused;
    DWORD recordNumber;
};

struct MftAttrHeader {
    DWORD type;
    DWORD length;
    BYTE nonResident;
    BYTE nameLen;
    WORD nameOffset;
    WORD flags;
    WORD attrId;
    DWORD valueLength;   // resident: value size
    WORD valueOffset;    // resident: value offset (from attr start)
    BYTE indexedFlag;
    BYTE pad;
};

// $FILE_NAME 属性
struct AttrFileName {
    LONGLONG parentRef;
    LARGE_INTEGER creation;
    LARGE_INTEGER modify;
    LARGE_INTEGER mftChange;
    LARGE_INTEGER access;
    LONGLONG allocSize;
    LONGLONG realSize;
    DWORD fileFlags;
    DWORD eaReparse;           // 0x3C 实测校准
    BYTE nameLen;
    BYTE nameSpace;
    WCHAR name[1];
};
#pragma pack(pop)

struct MftEntryInfo {
    ULONGLONG mftId;
    ULONGLONG parentMftId;
    std::wstring name;
    ULONGLONG size;
    bool isDir;
    bool skip;
};

// run list 直读：$MFT 数据在卷上的物理区间（LCN + 簇数）
struct MftRun {
    LONGLONG lcn;
    LONGLONG vcn;
    LONGLONG length;
};

// MFT 诊断日志：默认写 exe 同目录 diskmate.log（无需环境变量，所有用户都需要）
static void MftLog(const std::wstring& s) {
    wchar_t buf[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, buf, MAX_PATH)) return;
    std::wstring p = buf;
    size_t sl = p.find_last_of(L"\\/");
    if (sl == std::wstring::npos) return;
    p = p.substr(0, sl + 1) + L"diskmate.log";
    FILE* fp = _wfsopen(p.c_str(), L"a, ccs=UTF-8", _SH_DENYNO);
    if (fp) {
        fwprintf(fp, L"[MFT] %ls\n", s.c_str());
        fclose(fp);
    }
}


// 读取卷，获取MFT信息（大块顺序读，避免逐记录 Seek/Read 的巨量 IO 调用）
static ULONGLONG g_scanDirCount = 0;
// MFT 记录 fixup（USA）：raw 直读拿到的记录，每个 512B sector 末尾 2 字节
// 被更新序列号替换；解析属性前必须还原，否则 $FILE_NAME 等字段读出垃圾（ioctl 路径系统已代劳）
static void FixupRecord(BYTE* rec, DWORD recordSize) {
    auto* h = reinterpret_cast<MftFileRecordHeader*>(rec);
    if (h->magic != MFT_MAGIC) return;
    WORD usaOff = h->updateSeqOffset;
    WORD usaSize = h->updateSeqSize;
    if (usaSize < 2 || usaOff + usaSize * 2 > recordSize) return;
    for (WORD i = 1; i < usaSize; i++) {
        DWORD sectorEnd = (DWORD)i * 512 - 2;   // 每个 sector 末尾
        if (sectorEnd + 2 <= recordSize)
            *(WORD*)(rec + sectorEnd) = *(const WORD*)(rec + usaOff + i * 2);
    }
}

static bool ReadMftRecords(HANDLE hVolume,
                           std::vector<MftEntryInfo>& outEntries,
                           const std::atomic<bool>& cancel,
                           bool skipHidden,
                           const std::function<void(ULONGLONG, ULONGLONG, ULONGLONG)>& progressTick,
                           const std::wstring& volumeRootName)
{
    DWORD bytesReturned = 0;
    NTFS_VOLUME_DATA_BUFFER nvd{};
    if (!DeviceIoControl(hVolume, FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0,
        &nvd, sizeof(nvd), &bytesReturned, nullptr))
    {
        MftLog(L"FSCTL_GET_NTFS_VOLUME_DATA FAILED err=" + std::to_wstring(GetLastError()));
        return false;
    }
    DWORD recordSize = nvd.BytesPerFileRecordSegment ? nvd.BytesPerFileRecordSegment : FILE_RECORD_SEGMENT_SIZE;
    ULONGLONG mftCount = nvd.MftValidDataLength.QuadPart / recordSize;

    // 逐记录解析（全程边界检查）
    ULONGLONG dbg_noFn = 0, dbg_attrBrk = 0, dbg_zeroSize = 0;   // 诊断：解析失败统计
    std::vector<std::pair<ULONGLONG, ULONGLONG>> extMerge;   // (baseId, dataSize) 扩展记录 $DATA 合并
    ULONGLONG totalBytes = 0;   // 进度显示用（累计 realSize）
    auto parseRecord = [&](const BYTE* p) {
        auto* recHdr = reinterpret_cast<const MftFileRecordHeader*>(p);
        if (recHdr->magic != MFT_MAGIC) return;
        if ((recHdr->flags & 0x01) == 0) return;   // 未使用记录跳过
        if (recHdr->baseFileRec != 0) {
            // 扩展记录：不建独立条目，只把 $DATA 大小合并到主记录
            // （pagefile.sys / hiberfil.sys 等大文件的 $DATA 在扩展记录里）
            DWORD ap2 = recHdr->attrOffset;
                if (extMerge.size() < 30)
                    MftLog(L"extrec id=" + std::to_wstring((long long)recHdr->recordNumber) +
                           L" base=" + std::to_wstring((long long)recHdr->baseFileRec) + L" firstattr=" + std::to_wstring(*(const DWORD*)(p + ap2)));
            while (ap2 + sizeof(MftAttrHeader) <= recordSize && ap2 + sizeof(MftAttrHeader) <= recHdr->realSize) {
                const auto* attr2 = reinterpret_cast<const MftAttrHeader*>(p + ap2);
                if (attr2->type == 0xFFFFFFFF || attr2->length < sizeof(MftAttrHeader)) break;
                if (attr2->type == 0x80 && attr2->nameLen == 0) {
                {
                    if (extMerge.size() < 40)
                        MftLog(L"extDATA rec=" + std::to_wstring((long long)recHdr->recordNumber) +
                               L" base=" + std::to_wstring(((ULONGLONG)recHdr->baseFileRec) & 0x0000FFFFFFFFFFFFULL) +
                               L" nonres=" + std::to_wstring((int)attr2->nonResident) +
                               L" nameLen=" + std::to_wstring((int)attr2->nameLen) +
                               L" realSize=" + std::to_wstring((long long)(attr2->nonResident ? *(const LONGLONG*)(p + ap2 + 0x30) : (LONGLONG)attr2->valueLength)));
                }
                    ULONGLONG s2 = 0;
                    if (attr2->nonResident == 0) s2 = attr2->valueLength;
                    else if (ap2 + 0x38 <= recordSize) s2 = *(const LONGLONG*)(p + ap2 + 0x30);
                    if (s2) extMerge.emplace_back(((ULONGLONG)recHdr->baseFileRec) & 0x0000FFFFFFFFFFFFULL, s2);
                    break;
                }
                if (ap2 + attr2->length > recordSize) break;
                ap2 += attr2->length;
            }
            return;
        }

        MftEntryInfo entry{};
        entry.mftId = recHdr->recordNumber;
        entry.isDir = (recHdr->flags & 0x02) != 0;   // 记录头 flags 0x02 = 有 $INDEX_ROOT = 目录（权威，$FILE_NAME 不可靠）
        entry.skip = false;

        DWORD attrPos = recHdr->attrOffset;
        bool sawFn = false;
            // isDir 只用记录头 flags 0x02（权威）；$FILE_NAME.fileFlags / $SI 在 MFT 记录内可能为垃圾，不用于判断
        while (attrPos + sizeof(MftAttrHeader) <= recordSize &&
               attrPos + sizeof(MftAttrHeader) <= recHdr->realSize)
        {
            const auto* attr = reinterpret_cast<const MftAttrHeader*>(p + attrPos);
            if (recHdr->recordNumber == 123456 || recHdr->recordNumber == 253500) {
                MftLog(L"bigsys rec" + std::to_wstring((long long)recHdr->recordNumber) +
                       L" attr type=0x" + std::to_wstring(attr->type) +
                       L" len=" + std::to_wstring(attr->length) +
                       L" nonres=" + std::to_wstring(attr->nonResident) +
                       L" nameLen=" + std::to_wstring(attr->nameLen) +
                       L" usaOff=" + std::to_wstring(recHdr->updateSeqOffset) +
                       L" usaSz=" + std::to_wstring(recHdr->updateSeqSize));
                wchar_t hx2[256];
                const BYTE* ab = p + attrPos;
                swprintf(hx2, 256, L"  attrhex: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                         ab[0],ab[1],ab[2],ab[3],ab[4],ab[5],ab[6],ab[7],ab[8],ab[9],ab[10],ab[11],ab[12],ab[13],ab[14],ab[15]);
                MftLog(hx2);
            }
            if (attr->type == 0xFFFFFFFF) break;
            if (attr->length < sizeof(MftAttrHeader)) break;

            // isDir 只用记录头 flags 0x02（权威）；$FILE_NAME.fileFlags / $SI 在 MFT 记录内可能为垃圾，不用于判断

            // isDir 只用记录头 flags 0x02（权威）；$FILE_NAME.fileFlags / $SI 在 MFT 记录内可能为垃圾，不用于判断

            if (attr->type == 0x30 && attrPos + attr->length <= recordSize) // $FILE_NAME
            {
                sawFn = true;
                const auto* fnAttr = reinterpret_cast<const AttrFileName*>(p + attrPos + attr->valueOffset);
                entry.parentMftId = (ULONGLONG)(fnAttr->parentRef) & 0x0000FFFFFFFFFFFFULL;
                // 大小不从 $FILE_NAME 取：MFT 记录内该字段无意义（Windows 置 0/垃圾），真实大小在 $DATA 属性
                if (recHdr->recordNumber == 0 || entry.mftId == 0) {
                    wchar_t hx[512];
                    const BYTE* b = (const BYTE*)fnAttr;
                    swprintf(hx, 512, L"rec0 FN: parent=%llX alloc=%lld real=%lld flags=%X nameLen=%u name=%ls",
                             (unsigned long long)fnAttr->parentRef, (long long)fnAttr->allocSize,
                             (long long)fnAttr->realSize, fnAttr->fileFlags, fnAttr->nameLen, fnAttr->name);
                    MftLog(hx);
                    swprintf(hx, 512, L"rec0 FN hex: %02X %02X %02X %02X %02X %02X %02X %02X | %02X %02X %02X %02X %02X %02X %02X %02X",
                             b[0x28],b[0x29],b[0x2A],b[0x2B],b[0x2C],b[0x2D],b[0x2E],b[0x2F],
                             b[0x30],b[0x31],b[0x32],b[0x33],b[0x34],b[0x35],b[0x36],b[0x37]);
                    MftLog(hx);
                }
                if (entry.size && entry.name.length() <= 255) totalBytes += entry.size;


                DWORD nameByteLen = (DWORD)fnAttr->nameLen * sizeof(WCHAR);
                // 名字空间：0=POSIX 1=Win32(长名) 2=DOS(8.3短名) 3=Win32&DOS
                // 优先长名：DOS 短名只在该记录没有其它名字时兜底，
                // 否则 MFT 树会出现 PROGRA~1/WIF4A9~1 这类短名，与 walk 的长名对不上。
                bool isDos = (fnAttr->nameSpace == 2);
                if (fnAttr->nameLen <= 255 &&
                    attrPos + attr->valueOffset + offsetof(AttrFileName, name) + nameByteLen <= recordSize)
                {
                    if (!isDos || entry.name.empty()) {
                        entry.name.assign(fnAttr->name, fnAttr->nameLen);
                    }
                    if (entry.name == L"pagefile.sys" || entry.name == L"hiberfil.sys") {
                        MftLog(L"PFILE rec" + std::to_wstring((long long)recHdr->recordNumber) + L" name=" + entry.name +
                               L" isDir=" + std::to_wstring((int)entry.isDir));
                    }
                }
                if (skipHidden && (fnAttr->fileFlags & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
                    entry.skip = true;
            }
            if (attr->type == 0x80 && !entry.isDir && attr->nameLen == 0) {   // 主数据流大小（权威）
                if (attr->nonResident == 0)
                    entry.size = attr->valueLength;      // 驻留：值长度 = 文件大小
                else if (attrPos + 0x38 <= recordSize)
                    entry.size = *(const LONGLONG*)(p + attrPos + 0x30);   // 非驻留：RealSize
            }
            if (attrPos + attr->length > recordSize) { dbg_attrBrk++; break; }
            attrPos += attr->length;
        }

        if (!sawFn) dbg_noFn++;
        if (!entry.isDir && entry.size == 0) dbg_zeroSize++;
        if (entry.mftId == 5) entry.name = volumeRootName;   // 根目录固定用卷根名（NTFS 根 $FILE_NAME 名是 "."，不能当路径）
        if (!entry.name.empty() && !entry.skip)
            outEntries.push_back(std::move(entry));
                if (entry.isDir) g_scanDirCount++;
    };

    MftLog(L"parse diag: noFileName=" + std::to_wstring(dbg_noFn) +
           L" attrBreak=" + std::to_wstring(dbg_attrBrk) +
           L" zeroSizeFiles=" + std::to_wstring(dbg_zeroSize));
    // [对比] FSCTL（系统修复 fixup 后）读记录 0 的 $FILE_NAME realSize —— 校验 raw 数据
    {
        DWORD osz = sizeof(NTFS_FILE_RECORD_OUTPUT_BUFFER) + recordSize + 16;
        std::vector<BYTE> ob(osz);
        auto* o = reinterpret_cast<NTFS_FILE_RECORD_OUTPUT_BUFFER*>(ob.data());
        NTFS_FILE_RECORD_INPUT_BUFFER i2{};
        i2.FileReferenceNumber.QuadPart = 0;
        DWORD g2 = 0;
        if (DeviceIoControl(hVolume, FSCTL_GET_NTFS_FILE_RECORD, &i2, sizeof(i2), o, osz, &g2, nullptr)) {
            const BYTE* r0b = o->FileRecordBuffer;
            DWORD ap = ((const MftFileRecordHeader*)r0b)->attrOffset;
            while (ap + 24 <= recordSize) {
                DWORD ty = *(const DWORD*)(r0b + ap);
                DWORD al = *(const DWORD*)(r0b + ap + 4);
                if (ty == 0xFFFFFFFF || al < 24) break;
                if (ty == 0x30) {
                    if (ty == 0x80) {
                        if (r0b[ap + 8] == 1)  // 非驻留
                            MftLog(L"FSCTL rec0 DATA(nonres): alloc=" + std::to_wstring(*(const LONGLONG*)(r0b + ap + 0x28)) +
                                   L" real=" + std::to_wstring(*(const LONGLONG*)(r0b + ap + 0x30)));
                        else
                            MftLog(L"FSCTL rec0 DATA(res): len=" + std::to_wstring(*(const DWORD*)(r0b + ap + 0x10)));
                    }
                    const auto* fn = reinterpret_cast<const AttrFileName*>(r0b + ap + *(const WORD*)(r0b + ap + 0x14));
                    MftLog(L"FSCTL rec0 FN: alloc=" + std::to_wstring((long long)fn->allocSize) +
                           L" real=" + std::to_wstring((long long)fn->realSize) + L" name=" + std::wstring(fn->name, fn->nameLen));
                    break;
                }
                ap += al;
            }
        }
    }
    // ===== 首选：run list 大块直读（比逐条 ioctl 快 ~3 倍）=====
    bool usedRaw = false;
    {
        // 1) 读记录 0（$MFT 自身），解析其 $DATA 主数据流 run list
        DWORD outSize = sizeof(NTFS_FILE_RECORD_OUTPUT_BUFFER) + recordSize + 16;
        std::vector<BYTE> outBuf(outSize);
        auto* out = reinterpret_cast<NTFS_FILE_RECORD_OUTPUT_BUFFER*>(outBuf.data());
        NTFS_FILE_RECORD_INPUT_BUFFER in{};
        in.FileReferenceNumber.QuadPart = 0;
        DWORD got = 0;
        if (DeviceIoControl(hVolume, FSCTL_GET_NTFS_FILE_RECORD, &in, sizeof(in),
                            out, outSize, &got, nullptr)) {
            const BYTE* r0 = out->FileRecordBuffer;
            const auto* h0 = reinterpret_cast<const MftFileRecordHeader*>(r0);
            if (h0->magic == MFT_MAGIC) {
                // 2) 找 $DATA(0x80) 非驻留、nameLen==0 的属性 → run list
                DWORD attrPos = h0->attrOffset;
                struct DataCand { DWORD attrPos, alen; BYTE nameLen; WORD mapOff; LONGLONG lvc; };
                std::vector<DataCand> cands;
                while (attrPos + 24 <= h0->realSize && attrPos + 24 <= recordSize) {
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
                const BYTE* runList = nullptr;
                size_t runListCap = 0;
                LONGLONG lowestVcn = 0;
                for (auto& c : cands) {
                    if (c.nameLen == 0) {   // 主数据流
                        runList = r0 + c.attrPos + c.mapOff;
                        runListCap = c.alen - c.mapOff;
                        lowestVcn = c.lvc;
                        break;
                    }
                }
                if (!runList && !cands.empty()) {
                    runList = r0 + cands[0].attrPos + cands[0].mapOff;
                    runListCap = cands[0].alen - cands[0].mapOff;
                    lowestVcn = cands[0].lvc;
                }
                // 3) 解析 run list（低4位=length字节数，高4位=offset字节数 —— 实测校准）
                if (runList) {
                    std::vector<MftRun> runs;
                    size_t pos = 0;
                    LONGLONG curLcn = 0, curVcn = lowestVcn;
                    while (pos < runListCap) {
                        BYTE hdr = runList[pos];
                        if (hdr == 0) break;
                        int lenBytes = hdr & 0x0F;
                        int offBytes = hdr >> 4;
                        if (pos + 1 + lenBytes + offBytes > runListCap) break;
                        LONGLONG length = 0, off = 0;
                        for (int i = 0; i < lenBytes; i++)
                            length |= (LONGLONG)runList[pos + 1 + i] << (8 * i);
                        for (int i = 0; i < offBytes; i++) {
                            BYTE b = runList[pos + 1 + lenBytes + i];
                            if (i == offBytes - 1 && (b & 0x80))
                                off |= (LONGLONG)(signed char)b << (8 * i);
                            else
                                off |= (LONGLONG)b << (8 * i);
                        }
                        if (offBytes == 0) off = 0;
                        curLcn += off;
                        runs.push_back({curLcn, curVcn, length});
                        curVcn += length;
                        pos += 1 + lenBytes + offBytes;
                    }
                    // 4) 校验 run0 起点 == MftStartLcn；通过则大块读
                    if (!runs.empty() && runs[0].lcn == nvd.MftStartLcn.QuadPart) {
                        std::vector<BYTE> chunk(16 * 1024 * 1024);
                        ULONGLONG nRead = 0;
                        bool readFail = false;
                        for (auto& r : runs) {
                            if (cancel.load()) return false;
                            LONGLONG byteLen = r.length * nvd.BytesPerCluster;
                            LONGLONG done = 0;
                            LARGE_INTEGER off;
                            off.QuadPart = r.lcn * nvd.BytesPerCluster;
                            while (done < byteLen) {
                                size_t want = (size_t)std::min<LONGLONG>((LONGLONG)chunk.size(), byteLen - done);
                                DWORD rd = 0;
                                OVERLAPPED ov{};
                                ov.Offset = (DWORD)(off.QuadPart & 0xFFFFFFFF);
                                ov.OffsetHigh = (DWORD)((off.QuadPart >> 32) & 0xFFFFFFFF);
                                if (!ReadFile(hVolume, chunk.data(), (DWORD)want, &rd, &ov)) {
                                    readFail = true;
                                    break;
                                }
                                // raw 数据带 USA fixup：先还原再解析
                                for (size_t rec = 0; rec + recordSize <= rd; rec += recordSize)
                                    FixupRecord(chunk.data() + rec, recordSize);
                                for (size_t rec = 0; rec + recordSize <= rd; rec += recordSize) {
                                    parseRecord(chunk.data() + rec);
                                    nRead++;
                                }
                                done += rd;
                                off.QuadPart += rd;
                                if ((nRead & 0x3FFF) == 0 && progressTick)
                                    progressTick(outEntries.size(), totalBytes, 0);
                                if (rd == 0) { readFail = true; break; }
                            }
                            if (readFail) break;
                        }
                        if (!readFail && !cancel.load()) {
                            usedRaw = true;
                            MftLog(L"raw read done: runs=" + std::to_wstring(runs.size()) +
                                   L" entries=" + std::to_wstring(outEntries.size()));
                        }
                    }
                }
            }
        }
    }

    // ===== fallback：逐条 ioctl（raw 不可用时）=====
    if (!usedRaw) {
        outEntries.clear();
        g_scanDirCount = 0;
        totalBytes = 0;
        DWORD outSize = sizeof(NTFS_FILE_RECORD_OUTPUT_BUFFER) + recordSize + 16;
        std::vector<BYTE> outBuf(outSize);
        auto* out = reinterpret_cast<NTFS_FILE_RECORD_OUTPUT_BUFFER*>(outBuf.data());
        NTFS_FILE_RECORD_INPUT_BUFFER in{};

        // 记录5 = 根目录（NTFS 固定），特殊读入
        {
            in.FileReferenceNumber.QuadPart = 5;
            DWORD got = 0;
            if (DeviceIoControl(hVolume, FSCTL_GET_NTFS_FILE_RECORD, &in, sizeof(in),
                                out, outSize, &got, nullptr))
                parseRecord(out->FileRecordBuffer);
        }

        // 从后往前遍历（API 跳过无效记录：返回实际 ID，用它跳）
        ULONGLONG nRead = 0;
        LONGLONG index = (LONGLONG)mftCount - 1;
        while (index >= 16) {
            if (cancel.load()) return false;
            in.FileReferenceNumber.QuadPart = index;
            DWORD got = 0;
            if (!DeviceIoControl(hVolume, FSCTL_GET_NTFS_FILE_RECORD, &in, sizeof(in),
                                 out, outSize, &got, nullptr)) {
                index--;
                continue;
            }
            LONGLONG realIndex = out->FileReferenceNumber.QuadPart;
            parseRecord(out->FileRecordBuffer);
            nRead++;
            if ((nRead & 0x3FFF) == 0) {
                if (progressTick) progressTick(outEntries.size(), totalBytes, 0);
                MftLog(L"mft api: read=" + std::to_wstring(nRead) +
                       L" entries=" + std::to_wstring(outEntries.size()) +
                       L" idx=" + std::to_wstring(index));
            }
            if (realIndex <= 16) break;
            index = realIndex - 1;
        }
        MftLog(L"api read done: mftCount=" + std::to_wstring(mftCount) +
               L" entries=" + std::to_wstring(outEntries.size()));
    }
    // 合并扩展记录的 $DATA 大小到主记录（pagefile/hiberfil 等）
    if (!extMerge.empty()) {
        std::unordered_map<ULONGLONG, size_t> idIdx;
        idIdx.reserve(outEntries.size() * 2);
        for (size_t i = 0; i < outEntries.size(); i++) idIdx[outEntries[i].mftId] = i;
        for (auto& m : extMerge) {
            auto it = idIdx.find(m.first);
            if (it != idIdx.end()) outEntries[it->second].size += m.second;
        }
        MftLog(L"ext merged=" + std::to_wstring(extMerge.size()) + L" records, added " + std::to_wstring(extMerge.size()) + L" sizes");
    }
    if (progressTick) progressTick(outEntries.size(), totalBytes, 0);
    MftLog(L"dir count=" + std::to_wstring(g_scanDirCount) + L" of " + std::to_wstring(outEntries.size()));
    return true;
}

ScanResult ScanTreeMFT(const std::wstring& volumePath,
                       const std::atomic<bool>& cancel,
                       ScanStats* stats,
                       const std::function<void(ULONGLONG, ULONGLONG, ULONGLONG)>& progressTick,
                       int threads,
                       bool skipHidden,
                       bool followReparse)
{
    ScanResult res{};
    if (!stats) return res;
    *stats = {};

    //打开卷设备，需要管理员权限
    HANDLE hVolume = CreateFileW(volumePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0, nullptr);
    MftLog(L"volume opened OK: " + volumePath);

    if (hVolume == INVALID_HANDLE_VALUE)
    {
        MftLog(L"open volume FAILED: " + volumePath + L" err=" + std::to_wstring(GetLastError()));
        stats->cancelled = true;
        return res;
    }

    std::vector<MftEntryInfo> mftEntries;
    std::wstring volRoot = L"C:\\";
    {
        size_t cc = volumePath.rfind(L":");
        if (cc != std::wstring::npos && cc >= 1 && iswalpha(volumePath[cc - 1]))
            volRoot = std::wstring(1, (wchar_t)towupper(volumePath[cc - 1])) + L":\\";
    }
    bool readOk = ReadMftRecords(hVolume, mftEntries, cancel, skipHidden, progressTick, volRoot);
    CloseHandle(hVolume);

    if (cancel.load() || !readOk)
    {
        stats->cancelled = true;
        return res;
    }

    // MFT ID -> ScanNode映射
    std::unordered_map<ULONGLONG, ScanNode*> id2node;
    res.arena.reserve(mftEntries.size());

    //1. 全部节点预分配
    for (auto& e : mftEntries)
    {
        res.arena.emplace_back(std::make_unique<ScanNode>());
        ScanNode* nd = res.arena.back().get();
        nd->name = e.name;
        nd->isDir = e.isDir;
        nd->size = e.size;
        nd->fileCount = nd->isDir ? 0 : 1;
        id2node[e.mftId] = nd;
    }

    //2. 建立父子关系（统计孤儿：父记录缺失的节点）
    ULONGLONG orphanCount = 0;
    for (auto& e : mftEntries)
    {
        auto it = id2node.find(e.mftId);
        if (it == id2node.end()) continue;
        ScanNode* child = it->second;

        auto pit = id2node.find(e.parentMftId);
        if (pit != id2node.end() && pit->second != child)
        {
            // 跳过自引用：根目录（记录5）的 $FILE_NAME.parentRef 指向自身（"."幽灵节点）。
            // 若不跳过，根会挂成自己的子节点，聚合时把系统文件重复计入根大小。
            ScanNode* parent = pit->second;
            child->parent = parent;
            parent->children.push_back(child);
        }
        else if (pit == id2node.end())
        {
            orphanCount++;   // 父记录缺失（被过滤/损坏）→ 子树不计入根大小
        }
    }

    //3. 后序遍历一次性聚合size & fileCount（重点：移除旧代码边插入边向上累加，解决性能爆炸）
    std::unordered_set<ScanNode*> aggVisited;   // 防环：损坏记录可能形成引用环
    std::function<void(ScanNode*)> postAggregate;
    postAggregate = [&](ScanNode* nd)
    {
        if (!nd || !aggVisited.insert(nd).second) return;   // 防环
        for (ScanNode* ch : nd->children)
        {
            if (ch == nd) continue;   // 自引用（幽灵）防御：不能把自身大小累加进自身
            postAggregate(ch);
            nd->size += ch->size;
            nd->fileCount += ch->fileCount;
        }
    };

    // 寻找根目录MFT=5（NTFS固定：MFT记录5是根目录）
    auto rootIt = id2node.find(5);
    if (rootIt != id2node.end())
    {
        res.root = rootIt->second;
        MftLog(L"agg before: rootSize=" + std::to_wstring(res.root->size) +
               L" rootKids=" + std::to_wstring(res.root->children.size()));
        postAggregate(res.root);
        MftLog(L"root found id=5 name=" + res.root->name);
        MftLog(L"agg after: rootSize=" + std::to_wstring(res.root->size) +
               L" rootKids=" + std::to_wstring(res.root->children.size()));
    }
    else
    {
        MftLog(L"root NOT found (id=5 missing), MFT result discarded");
    }
    //统计填充
    stats->items = (ULONGLONG)mftEntries.size();
    ULONGLONG totalBytes = 0;
    if(res.root) totalBytes = res.root->size;
    ULONGLONG allBytes = 0;
    for (auto& e : mftEntries) allBytes += e.size;
    std::vector<const MftEntryInfo*> tops;
    for (auto& e : mftEntries) tops.push_back(&e);
    std::partial_sort(tops.begin(), tops.begin() + (tops.size() > 5 ? 5 : tops.size()), tops.end(),
                      [](const MftEntryInfo* a, const MftEntryInfo* b) { return a->size > b->size; });
    for (size_t ti = 0; ti < 5 && ti < tops.size(); ti++)
        MftLog(L"top file: " + std::to_wstring(tops[ti]->size) + L" " + tops[ti]->name);
    MftLog(L"diagnose: allBytes=" + std::to_wstring(allBytes) +
           L" rootBytes=" + std::to_wstring(totalBytes) +
           L" orphans=" + std::to_wstring(orphanCount) +
           L" dirs=" + std::to_wstring(g_scanDirCount));
    stats->bytes = totalBytes;
    stats->skipped = 0;
    stats->cancelled = cancel.load();

    if (progressTick)
    {
        progressTick(stats->items, stats->bytes, stats->skipped);
    }
    return res;
}
