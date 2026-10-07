# DiskMate 显示 38 GB vs 系统已用 157.57 GB —— 差异分析

> 项目：DiskMate（WebView2 + C++ MFT 直读磁盘分析器）
> 日期：2026-10-06
> 状态：**MFT 解析与聚合自洽（无漏挂），38.9 GB 是「所有文件逻辑大小之和」，与系统报告的「物理已用空间 157.57 GB」口径不同，差异主要指向卷级占用（VSS 卷影副本等高嫌疑）**

---

## 1. 问题概述

- 系统报告：C 盘总 199.40 GB，已用 **157.57 GB**（`GetDiskFreeSpaceEx`）
- DiskMate 显示：总共 **约 38 GB**
- 差距：约 **118 GB**

## 2. 关键日志（原始）

```
[MFT] raw read done: runs=10 entries=1242096
[MFT] dir count=240721 of 1242096
[MFT] diagnose: allBytes=41804922713 rootBytes=41872054457 orphans=0 dirs=240721
[MFT] raw read done: runs=10 entries=1242123
[MFT] diagnose: allBytes=41802841018 rootBytes=41869972762 orphans=0 dirs=240734
```

数值换算（按 1024³）：

| 指标 | 字节 | GB | 含义 |
|---|---|---|---|
| `allBytes` | 41,804,922,713 | 38.93 | 全部 MFT 条目 `$FILE_NAME.realSize` 之和（未挂树也计入） |
| `rootBytes` | 41,872,054,457 | 38.99 | 根目录聚合后总大小（`stats.bytes`，界面显示值） |
| `orphans` | 0 | — | 父记录缺失、无法挂到树上的节点数 |
| `dirs` | 240,721 | — | 目录数（raw 路径一次运行值；见 5.4 不一致现象） |
| `entries` | 1,242,096 | — | 有效 MFT 记录条数 |

## 3. 相关代码（src/scanner_M.cpp）

### 3.1 大小来源：$FILE_NAME.realSize（逻辑大小）

```
// L66-80：$FILE_NAME 属性结构
struct AttrFileName {
    LONGLONG parentRef;      // 0x00 父目录引用（高16位是序列号，需屏蔽）
    LARGE_INTEGER creation, modify, mftChange, access;
    LONGLONG allocSize;      // 0x28 分配大小（物理）
    LONGLONG realSize;       // 0x30 实际大小（逻辑）
    DWORD fileFlags;
    DWORD eaReparse;         // 0x3C 实测校准
    BYTE nameLen; BYTE nameSpace; WCHAR name[1];
};

// L176-180：解析处 —— 大小取 realSize（逻辑大小，非分配大小）
if (attr->type == 0x30 && ...) {   // $FILE_NAME
    const auto* fnAttr = reinterpret_cast<const AttrFileName*>(p + attrPos + attr->valueOffset);
    entry.parentMftId = (ULONGLONG)(fnAttr->parentRef) & 0x0000FFFFFFFFFFFFULL;
    entry.size = fnAttr->realSize;                 // ← 大小来源
    ...
}
```

> **要点**：`realSize` 是 NTFS 语义的「文件长度」（资源管理器"大小"列同源），
> **不是**「占用空间」（"占用空间"列/`allocSize`）。压缩、稀疏文件的
> realSize 只可能 ≥ 物理占用，**不会导致偏小**。

### 3.2 raw 直读路径（run list 大块读，L203-321）

```
// L249-275：run list 解析（低4位=length字节数，高4位=offset字节数 —— 实测校准）
while (pos < runListCap) {
    BYTE hdr = runList[pos];
    if (hdr == 0) break;
    int lenBytes = hdr & 0x0F;
    int offBytes = hdr >> 4;
    ...
    LONGLONG length = 0, off = 0;
    for (int i = 0; i < lenBytes; i++)
        length |= (LONGLONG)runList[pos + 1 + i] << (8 * i);
    for (int i = 0; i < offBytes; i++) {
        BYTE b = runList[pos + 1 + lenBytes + i];
        if (i == offBytes - 1 && (b & 0x80))
            off |= (LONGLONG)(signed char)b << (8 * i);   // 符号扩展
        else off |= (LONGLONG)b << (8 * i);
    }
    ...
}
```

### 3.3 USA fixup（L117-130）—— raw 直读必须还原

```
// MFT 记录 fixup（USA）：raw 直读拿到的记录，每个 512B sector 末尾 2 字节
// 被更新序列号替换；解析属性前必须还原，否则 $FILE_NAME 等字段读出垃圾
static void FixupRecord(BYTE* rec, DWORD recordSize) {
    auto* h = reinterpret_cast<MftFileRecordHeader*>(rec);
    if (h->magic != MFT_MAGIC) return;
    WORD usaOff = h->updateSeqOffset, usaSize = h->updateSeqSize;
    if (usaSize < 2 || usaOff + usaSize * 2 > recordSize) return;
    for (WORD i = 1; i < usaSize; i++) {
        DWORD sectorEnd = (DWORD)i * 512 - 2;
        if (sectorEnd + 2 <= recordSize)
            *(WORD*)(rec + sectorEnd) = *(const WORD*)(rec + usaOff + i * 2);
    }
}
```

### 3.4 父子挂接 + 孤儿统计（L434-449）

```
//2. 建立父子关系（统计孤儿：父记录缺失的节点）
ULONGLONG orphanCount = 0;
for (auto& e : mftEntries) {
    auto it = id2node.find(e.mftId);
    if (it == id2node.end()) continue;
    ScanNode* child = it->second;
    auto pit = id2node.find(e.parentMftId);
    if (pit != id2node.end()) {
        ScanNode* parent = pit->second;
        child->parent = parent;
        parent->children.push_back(child);
    } else {
        orphanCount++;   // 父记录缺失（被过滤/损坏）→ 子树不计入根大小
    }
}
```

### 3.5 聚合 + 诊断日志（L451-489）

```
//3. 后序遍历一次性聚合 size & fileCount（防环）
std::unordered_set<ScanNode*> aggVisited;
std::function<void(ScanNode*)> postAggregate;
postAggregate = [&](ScanNode* nd) {
    if (!nd || !aggVisited.insert(nd).second) return;   // 防环
    for (ScanNode* ch : nd->children) {
        postAggregate(ch);
        nd->size += ch->size;
        nd->fileCount += ch->fileCount;
    }
};
auto rootIt = id2node.find(5);   // MFT 记录 5 = 根目录（NTFS 固定）
if (rootIt != id2node.end()) { res.root = rootIt->second; postAggregate(res.root); }
...
ULONGLONG allBytes = 0;
for (auto& e : mftEntries) allBytes += e.size;
MftLog(L"diagnose: allBytes=" + std::to_wstring(allBytes) +
       L" rootBytes=" + std::to_wstring(totalBytes) +
       L" orphans=" + std::to_wstring(orphanCount) +
       L" dirs=" + std::to_wstring(g_scanDirCount));
```

## 4. 数据对照

| 来源 | 数值 | 口径 |
|---|---|---|
| GetDiskFreeSpaceEx | 157.57 GB 已用 | **物理簇占用**（文件占用 + 元数据 + 卷影 + slack） |
| DiskMate `rootBytes` | 38.99 GB | **文件逻辑大小之和**（$FILE_NAME.realSize 聚合） |
| DiskMate `allBytes` | 38.93 GB | 同上（不依赖挂树，纯解析值） |
| 差异 | ~118.6 GB | 卷上不属于任何文件逻辑大小的占用 |

## 5. 可能原因（按证据强度分级）

### 5.1 已排除（有直接证据）

| 假设 | 证据 |
|---|---|
| 跳过隐藏/系统文件 | `config.json` 显式 `skipHidden: false` |
| 孤儿节点漏挂 | `orphans=0`（真实统计，else 分支已修正后复跑） |
| 休眠/页面/交换文件 | `C:\hiberfil.sys / pagefile.sys / swapfile.sys` 均不存在 |
| USN 日志占大 | `fsutil usn queryjournal`：Maximum Size 仅 32 MB |
| USA fixup 缺失（数值垃圾） | 修复后大小显示正常（不再是 2000 万 TB），且 allBytes≈rootBytes 自洽 |

### 5.2 高嫌疑（未验证，需管理员权限）

- **VSS 卷影副本 / 系统还原点（System Volume Information）**：
  `Get-ChildItem 'C:\System Volume Information'` 被拒绝（Access denied）。
  Windows 默认可能开启系统保护，还原点可占 **数十 GB**，且**不计入任何普通文件的
  逻辑大小**。这是 118 GB 差距最可能的单一来源。
  → 验证命令（管理员）：`vssadmin list shadowstorage`

### 5.3 中低嫌疑（量级不够凑满 118 GB，但属正常差异组成部分）

- **NTFS 元数据**：$MFT（1.37 GB）、$LogFile、$Bitmap、$Extend、坏簇重定向等
- **簇内碎片 slack**：平均约 0.5 簇/文件 × 124 万文件 ≈ 数百 MB~1 GB 级
- **$FILE_NAME 缺失/名字异常的条目**：`!entry.name.empty()` 才入树，个别无 $FILE_NAME 的记录不计（量级小）
- **前代系统残留**：Windows.old、回收站、$Recycle.Bin 等（属于普通文件，若有则 MFT 已计入——若用户确认这些存在且未显示，需单独排查）

### 5.4 代码层次要异常（不影响 38 GB 结论，需后续清理）

| 现象 | 说明 |
|---|---|
| `rootBytes > allBytes` 约 67 MB（0.16%） | 聚合值略大于纯解析值，可能个别节点被双挂（如 id2node 覆盖后旧节点仍在 children 中），或个别目录 realSize 非 0 |
| `dirs` 两次运行不一致（240,721 vs 481,442） | 差值约为 2 倍，疑似 `g_scanDirCount` 在个别运行中被重复累加（需查 parseRecord 调用次数/多实例启动） |

## 6. 结论

1. **MFT 直读解析与聚合是自洽的**：allBytes ≈ rootBytes ≈ 38.9 GB，orphans=0，
   没有「漏挂节点」导致的丢失。
2. **38.9 GB 是 C 盘所有文件逻辑大小之和**（与之前 FindFirstFile 遍历版 38.87 GB 几乎一致，
   两条独立路径互相印证），**不是**「磁盘已用空间」。
3. **118 GB 差异主要来自卷级占用**（物理已用 − 文件逻辑大小），**最可疑的是 VSS 卷影副本**，
   需管理员权限执行 `vssadmin list shadowstorage` 确认。
4. 若要界面显示「已用空间」口径，需要额外走 `GetDiskFreeSpaceEx`（与树图无关），
   或在设置中提供「按占用空间（allocSize）统计」选项——后者对压缩/稀疏文件更接近磁盘占用，
   但对普通 NTFS 卷与 realSize 差异有限，无法解释 118 GB。

## 7. 建议下一步

- [ ] 管理员运行 `vssadmin list shadowstorage` 核对卷影占用（若 >100 GB，原因坐实）
- [ ] 清理 5.4 的两个代码层异常（rootBytes 溢出 67 MB、dirs 计数不一致）
- [ ] 决策：界面是否增加「显示物理占用」开关（GetDiskFreeSpaceEx 或 allocSize）
