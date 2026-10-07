# DiskMate MFT 直读 vs 普通遍历：总大小缺口诊断（2026-10-06）

## 1. 问题概述

MFT 直读模式（`scanner_M.cpp`）与普通目录遍历（`scanner.cpp`）扫描同一 C 盘，**总大小不一致**：

| 模式 | 总大小 | 说明 |
|---|---|---|
| 普通遍历 walk（`scan_test.exe C:\`） | **157.8 GB**（169,488,439,055 B） | 与系统 GetDiskFreeSpaceEx 已用 157.57 GB 吻合，视为基准正确 |
| 普通遍历 walk（UI dump） | **169.5 GB**（169,514,931,481 B） | dump 口径含目录聚合，略高于 scan_test |
| MFT 直读（当前最新版） | **132.07 GB**（132,067,376,887 B） | 仍差 **~37.4 GB** |

系统值：C 盘总 199.40 GB / 已用 157.57 GB（GetDiskFreeSpaceEx）。

---

## 2. 已确诊并修复的问题（按时间顺序）

### 2.1 USA fixup 缺失（已修复）
- **现象**：raw 直读记录每 512B sector 末尾 2 字节被更新序列号替换 → 字段读出垃圾（早期"两千多万 TB"）。
- **修复**：`FixupRecord()` 在解析前按 USA 数组还原 sector 末尾。

### 2.2 $FILE_NAME.realSize 在 MFT 记录内无意义（已修复，核心）
- **现象**：早期 MFT 总大小只有 38.9 GB（= 所有文件 $FILE_NAME.realSize 之和）；$MFT 自身读出 16384（16KB）。
- **根因**：NTFS 规则——$FILE_NAME 的 allocSize/realSize 只在**目录索引（INDX）**中有意义；在 **MFT 记录内部**被置为 0 或垃圾值，真实大小在 **$DATA 属性**：
  - 驻留 $DATA：`valueLength`（属性头 +0x10）
  - 非驻留 $DATA：`realSize`（属性头 +0x30）
- **修复**：大小改从主数据流 $DATA（`type==0x80 && nameLen==0`）取值。
- **验证**：$MFT 记录 0 的 $DATA realSize = **1,372,848,128（1.37 GB）**，正确。rootSize 从 41.8 GB 跳到 ~131.9 GB。

### 2.3 isDir 判断只用记录头 flags 0x02（已修复）
- $FILE_NAME.fileFlags / $STANDARD_INFORMATION 在 MFT 记录内也可能为垃圾，不能用于目录判断。
- 修复后 rootSize 无变化（排除为主因）。

### 2.4 属性类型"0x16/0x32/0x48"是十进制打印陷阱（澄清，非 bug）
- 日志格式 `attr type=0x" + std::to_wstring(attr->type)`：`0x` 后跟的是**十进制**数字。
- `0x16` = 十进制 16 = 0x10（$STANDARD_INFORMATION）；`0x32` = 十进制 32 = 0x20（$ATTRIBUTE_LIST）；`0x48` = 十进制 48 = 0x30（$FILE_NAME）；`0x128` = 十进制 128 = 0x80（$DATA）。
- **hex dump 证实内存字节完全正确**（`10 00 00 00`、`20 00 00 00`、`30 00 00 00`），属性解析从未错位。

---

## 3. 当前剩余缺口：~37.4 GB 的头号嫌疑

### 3.1 直接证据：pagefile.sys / hiberfil.sys 大小 = 0
最新 MFT dump（root.name="."）顶层：

| 文件 | walk 正确值 | MFT 直读当前值 |
|---|---|---|
| pagefile.sys | 7,247,757,312（7.24 GB） | **0** |
| hiberfil.sys | 6,781,059,072（6.78 GB） | **0** |
| $MFT | 1,372,848,128（1.37 GB） | 1,372,848,128 ✓ |
| swapfile.sys | 16,777,216（16 MB） | 16,777,216 ✓ |

两个文件合计 **14.0 GB**，是最直接的缺口；其余 ~23 GB 待同类扩展记录问题确认后复查。

### 3.2 根因：这两个文件带 $ATTRIBUTE_LIST，$DATA 在扩展记录里

rec123456（pagefile.sys）属性遍历结果：

| 属性类型 | 长度 | 驻留 | 含义 |
|---|---|---|---|
| 0x10 | 96 | 是 | $STANDARD_INFORMATION |
| 0x20 | 120 | 是 | **$ATTRIBUTE_LIST** ← 关键 |
| 0x30 | 120 | 是 | $FILE_NAME |
| —— | —— | —— | **无 $DATA** |

rec253500（hiberfil.sys）同理：0x10 / **0x20（非驻留）** / 0x30，无 $DATA。

**结论**：当文件带 $ATTRIBUTE_LIST（属性过多或数据过大），真正的 $DATA 属性被移动到**扩展记录**（`baseFileRec != 0` 的记录）。旧代码 `if (recHdr->baseFileRec != 0) return;` 直接跳过扩展记录 → $DATA 永远读不到 → size=0。

### 3.3 扩展记录数据（已打日志确认）

扩展记录解析日志（节选）：

```
extrec id=162  base=237001930390565121 firstattr=128   (128 = 0x80 $DATA)
extrec id=314  base=5629499534638833   firstattr=128
extrec id=505  base=281474976743684    firstattr=48    (48 = 0x30 $FILE_NAME)
extrec id=31261 base=281474976740860  firstattr=128
...
```

- 绝大多数扩展记录**第一个属性就是 $DATA（128）**。
- **陷阱**：`baseFileRec` 是 64 位文件引用（低 48 位 = 记录号，高 16 位 = 序列号）。与主记录 `mftId`（32 位纯记录号）匹配必须取低 48 位：
  ```cpp
  ULONGLONG baseRec = ((ULONGLONG)recHdr->baseFileRec) & 0x0000FFFFFFFFFFFFULL;
  ```

### 3.4 已实现但未生效：扩展记录 $DATA 合并

已改代码：
1. `parseRecord` 中 baseFileRec != 0 的记录不再跳过，而是遍历其 $DATA（`type==0x80 && nameLen==0`），取 realSize 存入 `extMerge`（pair<baseRec低48位, size>）。
2. 解析结束后按 `mftId` 建立索引，把扩展记录大小累加到主记录。

**当前状态**：日志未见 `ext merged=` 行，rootSize 仍为 132.07 GB → 合并实际未生效。**待确认**：
- 扩展记录 $DATA 的 `nameLen` 是否 == 0（若为命名流则被条件排除）；
- 扩展记录 $DATA 非驻留 realSize 是否 > 0（`ap2 + 0x38 <= recordSize` 边界）；
- 合并代码插入位置是否正确执行（可能在 fallback 分支遗漏）。

---

## 4. 关键日志数据（2026-10-06 最新一轮）

```
[MFT] bigsys rec123456 attr type=0x16 len=96 nonres=0 nameLen=0 usaOff=48 usaSz=3
[MFT]   attrhex: 10 00 00 00 60 00 00 00 00 00 00 00 00 00 00 00
[MFT] bigsys rec123456 attr type=0x32 len=120 nonres=0 nameLen=0
[MFT]   attrhex: 20 00 00 00 78 00 00 00 00 00 00 00 00 00 03 00
[MFT] bigsys rec123456 attr type=0x48 len=120 nonres=0 nameLen=0
[MFT]   attrhex: 30 00 00 00 78 00 00 00 00 00 00 00 00 00 02 00
[MFT] PFILE rec123456 name=pagefile.sys isDir=0
[MFT] bigsys rec253500 attr type=0x16 len=96 nonres=0 nameLen=0
[MFT]   attrhex: 10 00 00 00 60 00 00 00 00 00 00 00 00 00 00 00
[MFT] bigsys rec253500 attr type=0x32 len=72 nonres=1 nameLen=0
[MFT]   attrhex: 20 00 00 00 48 00 00 00 01 00 00 00 00 00 04 00
[MFT] bigsys rec253500 attr type=0x48 len=120 nonres=0 nameLen=0
[MFT]   attrhex: 30 00 00 00 78 00 00 00 00 00 00 00 00 00 02 00
[MFT] PFILE rec253500 name=hiberfil.sys isDir=0
[MFT] raw read done: runs=10 entries=1242251
[MFT] dir count=240726 of 1242251
[MFT] agg before: rootSize=0 rootKids=45
[MFT] root found id=5 name=.
[MFT] agg after: rootSize=132067376887 rootKids=45
[MFT] top file: 2382364672 {6bad1d9a-...}
[MFT] top file: 1372848128 $MFT
[MFT] top file: 1016383933 model_file.pth.tar
[MFT] top file: 908500992 1d2a6b78.msi
[MFT] top file: 908500992 FoxitSetup.msi
[MFT] diagnose: allBytes=130627413239 rootBytes=132067376887 orphans=0 dirs=240726
```

（注：`type=0x16/0x32/0x48` 均为十进制打印，实际为 0x10/0x20/0x30，见 2.4。）

---

## 5. 结论与下一步

**结论**：
1. 属性解析逻辑正确（hex 证实）；
2. 剩余 37.4 GB 缺口的主因是 **pagefile.sys（7.24 GB）+ hiberfil.sys（6.78 GB）的 $DATA 在扩展记录中未被计入**（合计 14 GB）；
3. 其余 ~23 GB 需在扩展记录合并生效后复测确认（可能还有其它带 $ATTRIBUTE_LIST 的大文件，或命名数据流/压缩文件等次要因素）。

**下一步**：
1. 确认扩展记录 $DATA 解析条件（nameLen、realSize、边界）为何未产生合并；
2. 修复合并路径并复测 rootSize 是否追上 ~146 GB；
3. 若仍差 ~23 GB，继续比对 MFT 与 walk 的顶层目录大小（Users/Windows/Program Files…）逐层定位。
