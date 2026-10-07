// mftprobe.cpp — 独立 MFT 直读探针（先跑通，再整合回 scanner_M）
// 用法：管理员运行 mftprobe.exe   （固定探测 C:）
// 输出写 mftprobe_out.txt（UTF-8）
#include <windows.h>
#include <winioctl.h>
#include <cstdio>
#include <string>
#include <vector>
#include <cwchar>
#include <cstdlib>

#ifndef FILE_RECORD_SEGMENT_SIZE
#define FILE_RECORD_SEGMENT_SIZE 1024
#endif
#define MFT_MAGIC 0x454C4946 // "FILE"

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
struct MftAttrHeader {
    DWORD type;
    DWORD length;
    BYTE nonResident;
    BYTE nameLen;
    WORD nameOffset;
    WORD flags;
    WORD attrId;
    DWORD valueLength;
    WORD valueOffset;
    BYTE indexedFlag;
    BYTE pad;
};
// $FILE_NAME（实测 dump 校准）：allocSize@0x28, realSize@0x30,
// flags@0x38, EA@0x3C, nameLen@0x40, nameSpace@0x41, name@0x42
struct AttrFileName {
    LONGLONG parentRef;        // 0x00
    LARGE_INTEGER creation;    // 0x08
    LARGE_INTEGER modify;      // 0x10
    LARGE_INTEGER mftChange;   // 0x18
    LARGE_INTEGER access;      // 0x20
    LONGLONG allocSize;        // 0x28
    LONGLONG realSize;         // 0x30
    DWORD fileFlags;           // 0x38
    DWORD eaReparse;           // 0x3C
    BYTE nameLen;              // 0x40
    BYTE nameSpace;            // 0x41
    WCHAR name[1];             // 0x42
};
#pragma pack(pop)

struct Entry {
    ULONGLONG mftId, parentMftId, size;
    std::wstring name;
    bool isDir;
};

static std::string W2U8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static FILE* gOut;

int main() {
    gOut = fopen("mftprobe_out.txt", "wb");
    if (!gOut) return 2;
#define printf(...) fprintf(gOut, __VA_ARGS__)

    HANDLE h = CreateFileW(L"\\\\.\\C:", GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        printf("OPEN VOLUME FAILED err=%lu (need admin?)\n", GetLastError());
        fclose(gOut);
        return 1;
    }
    printf("volume opened OK\n");
    printf("struct offsets: parentRef=%zu realSize=%zu fileFlags=%zu nameLen=%zu name=%zu sizeof=%zu\\n",
           offsetof(AttrFileName, parentRef), offsetof(AttrFileName, realSize),
           offsetof(AttrFileName, fileFlags), offsetof(AttrFileName, nameLen),
           offsetof(AttrFileName, name), sizeof(AttrFileName));

    DWORD br = 0;
    NTFS_VOLUME_DATA_BUFFER nvd{};
    if (!DeviceIoControl(h, FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0,
                         &nvd, sizeof(nvd), &br, nullptr)) {
        printf("FSCTL_GET_NTFS_VOLUME_DATA FAILED err=%lu\n", GetLastError());
        fclose(gOut);
        return 1;
    }
    LONGLONG mftStartByte = nvd.MftStartLcn.QuadPart * nvd.BytesPerCluster;
    DWORD recordSize = nvd.BytesPerFileRecordSegment ? nvd.BytesPerFileRecordSegment : FILE_RECORD_SEGMENT_SIZE;
    ULONGLONG totalRecords = nvd.MftValidDataLength.QuadPart / recordSize;
    ULONGLONG mftBytes = totalRecords * recordSize;
    printf("MftStartLcn=%lld BytesPerCluster=%lu BpFRS=%lu MftValidDataLength=%llu\n",
           nvd.MftStartLcn.QuadPart, nvd.BytesPerCluster, nvd.BytesPerFileRecordSegment,
           nvd.MftValidDataLength.QuadPart);
    printf("mftStartByte=%lld recordSize=%lu totalRecords=%llu mftBytes=%llu\n",
           mftStartByte, recordSize, totalRecords, mftBytes);

    const DWORD CHUNK = 1 << 20;
    BYTE* chunk = (BYTE*)VirtualAlloc(nullptr, CHUNK, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!chunk) { printf("VirtualAlloc fail\n"); fclose(gOut); return 1; }

    ULONGLONG nBlocks = 0, nPosFail = 0, nReadFail = 0;
    ULONGLONG nMagic = 0, nUsed = 0, nBase0 = 0, nAttr30 = 0, nBadLen = 0, nAssigned = 0, nEmptyName = 0;
    std::vector<Entry> entries;
    const ULONGLONG MAX_BLOCKS = 20;

    for (ULONGLONG off = 0; off < mftBytes && off < MAX_BLOCKS * CHUNK; off += CHUNK) {
        nBlocks++;
        LARGE_INTEGER pos{};
        pos.QuadPart = mftStartByte + off;
        if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) { nPosFail++; continue; }
        DWORD want = (DWORD)((off + CHUNK <= mftBytes) ? CHUNK : (mftBytes - off));
        DWORD got = 0;
        if (!ReadFile(h, chunk, want, &got, nullptr)) {
            nReadFail++;
            printf("  [block %llu off=%llu] ReadFile FAILED err=%lu\n", nBlocks, off, GetLastError());
            continue;
        }
        if (got != want)
            printf("  [block %llu off=%llu] short read got=%lu want=%lu\n", nBlocks, off, got, want);

        size_t nRec = got / recordSize;
        for (size_t i = 0; i < nRec; i++) {
            const BYTE* p = chunk + i * recordSize;
            const auto* rh = reinterpret_cast<const MftFileRecordHeader*>(p);
            if (rh->magic != MFT_MAGIC) continue;
            nMagic++;
            if ((rh->flags & 0x01) == 0) continue;
            nUsed++;
            if (rh->baseFileRec != 0) continue;
            nBase0++;

            if (rh->recordNumber == 0) {
                printf("  --- rec0 attr loop ---\\n");
                DWORD ap2 = rh->attrOffset;
                while (ap2 + sizeof(MftAttrHeader) <= recordSize) {
                    const auto* at2 = reinterpret_cast<const MftAttrHeader*>(p + ap2);
                    if (at2->type == 0xFFFFFFFF) break;
                    printf("    attr@%u type=0x%02X len=%lu valueOff=%u\\n", ap2, at2->type, at2->length, at2->valueOffset);
                    if (at2->type == 0x30) {
                        const auto* fn2 = reinterpret_cast<const AttrFileName*>(p + ap2 + at2->valueOffset);
                        printf("      fn: parent=%llX real=%lld flags=%08X nameLen=%u ns=%u namePtr=%p\\n",
                               (unsigned long long)fn2->parentRef, (long long)fn2->realSize, fn2->fileFlags,
                               fn2->nameLen, fn2->nameSpace, (const void*)fn2->name);
                        const BYTE* np = (const BYTE*)fn2->name;
                        printf("      name bytes:");
                        for (int bx = 0; bx < 20; bx++) printf(" %02X", np[bx]);
                        printf("\\n");
                    }
                    if (at2->length < sizeof(MftAttrHeader)) break;
                    ap2 += at2->length;
                }
            }
            Entry e{};
            e.mftId = rh->recordNumber;
            DWORD attrPos = rh->attrOffset;
            while (attrPos + sizeof(MftAttrHeader) <= recordSize &&
                   attrPos + sizeof(MftAttrHeader) <= rh->realSize) {
                const auto* at = reinterpret_cast<const MftAttrHeader*>(p + attrPos);
                if (at->type == 0xFFFFFFFF) break;
                if (at->length < sizeof(MftAttrHeader)) break;
                if (at->type == 0x30 && attrPos + at->length <= recordSize) {
                    nAttr30++;
                    const auto* fn = reinterpret_cast<const AttrFileName*>(p + attrPos + at->valueOffset);
                    e.parentMftId = (ULONGLONG)(fn->parentRef) & 0x0000FFFFFFFFFFFFULL;
                    e.size = fn->realSize;
                    e.isDir = !!(fn->fileFlags & FILE_ATTRIBUTE_DIRECTORY);
                    DWORD nb = (DWORD)fn->nameLen * sizeof(WCHAR);
                    if (fn->nameLen <= 255 &&
                        attrPos + at->valueOffset + offsetof(AttrFileName, name) + nb <= recordSize) {
                        e.name.assign(fn->name, fn->nameLen);
                    } else {
                        nBadLen++;
                    }
                }
                if (attrPos + at->length > recordSize) break;
                attrPos += at->length;
            }
            if (!e.name.empty()) {
                nAssigned++;
                if (entries.size() < 5) entries.push_back(e);
            } else {
                nEmptyName++;
            }
        }
    }

    printf("\nSUMMARY blocks=%llu posFail=%llu readFail=%llu magic=%llu used=%llu base0=%llu entries=%llu\n",
           nBlocks, nPosFail, nReadFail, nMagic, nUsed, nBase0, nAssigned);
    printf("  attr30=%llu badLen=%llu emptyName=%llu\n", nAttr30, nBadLen, nEmptyName);
    printf("sample entries:\n");
    for (size_t i = 0; i < entries.size(); i++)
        printf("  [%zu] id=%llu parent=%llu dir=%d size=%llu name=%s\n",
               i, entries[i].mftId, entries[i].parentMftId, (int)entries[i].isDir,
               entries[i].size, W2U8(entries[i].name).c_str());

    VirtualFree(chunk, 0, MEM_RELEASE);
    CloseHandle(h);
    printf("DONE\n");
    fclose(gOut);
    return 0;
}
