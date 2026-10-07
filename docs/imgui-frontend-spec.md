# DiskMate ImGui 前端 —— 与 HTML 版对齐清单（复核版）

> 基准：`web/index.html`（3423 行，全文件逐行读过；本文件是**唯一校验基准**）
> 对照实现：`src_imgui/`（main_imgui.cpp / treemap_panel.cpp / settings_modal.cpp / shell_menu.cpp）
> 复核时间：2026-10-07 晚（本轮对齐工作完成后）
> 状态图例：✅ 已对齐 / 🔶 部分 / ❌ 未实现 / ➖ 不适用（架构差异）/ ⚠️ 有意保留的差异

## 〇、本轮复核结论（先说结果）

* 树图**布局算法**已逐项复刻并做了**数值比对**：同一棵扫描树、同一画布尺寸下，
  C++ 与 HTML 的 JS 得到**完全相同的叶子数 / 目录数 / 前 5 大块面积**（±1 px² 取整误差）。
* 列表、树图、工具栏、面包屑、状态栏、设置模态、右键壳菜单、滚轮框选、联动定位、
  过渡动画全部落地，并逐项截图自检（见第九节）。
* 原清单里大量标 ❌ 的项在本轮之前其实已经做完（清单未同步）；本轮把**真正缺失**的补齐，
  并把状态表更新为实测口径。

## 一、工具栏

| # | 功能 | 状态 | 实现/备注 |
|---|------|------|-----------|
| T1 | ← 返回（导航历史栈，上限 200） | ✅ | `GoBack()`；栈空时按钮 Disabled |
| T2 | ↑ 上级 | ✅ | `GoUp()` |
| T3 | 路径输入框 | ✅ | InputText，flex 宽度（按其余控件实测宽度反算） |
| T4 | 浏览（目录选择） | ✅ | `SHBrowseForFolderW`，选完写回输入框并落盘 lastPath |
| T5 | 扫描/停止（停止时红色 danger + 主按钮蓝底） | ✅ | 扫描中 `button.danger` #c0392b、「停止」；空闲为 accent 蓝「扫描」 |
| T6 | 映射 g 下拉（id/log/log2/sqrt/pow） | ✅ | 文案与 HTML 一致（`映射 g=√x` 等 5 项） |
| T7 | 运算顺序下拉（sumG/Gsum） | ✅ | `顺序 先和→g(Σx)` / `顺序 先 g→Σg(x)` |
| T8 | 幂指数 α 滑条（pow 时显示） | ✅ | 90px 滑条 + 右侧数值文本（两位小数），仅在 `mapKind==pow` 显示 |
| T9 | 主题切换（浅/深，文案联动 + 立即重渲） | ✅ | 文案 `浅色`/`深色`；切换后重建 ImGui 样式与树图配色 |
| T10 | 设置按钮（⚙ 设置） | ✅ | 齿轮字形通过合并 Segoe UI Symbol 得到（雅黑缺 U+2699） |
| T11 | MFT 模式切换（双向绑定 + 提权 + 失败回退） | ✅ | 显示 = 管理员 && mode≠0；点「普通遍历」→ 请求提权（runas 重启接管）→ 用户拒绝则回退普通遍历 |
| — | 出厂默认面积算法 | ✅ | `mapKind=3(√x)` + `ordKind=0(先和→g)`，与 HTML 出厂默认一致（`settings.h` 已同步） |

## 二、面包屑

| # | 功能 | 状态 | 备注 |
|---|------|------|------|
| C1 | 根→当前链，`›` 分隔，点击跳转（可被「返回」撤销） | ✅ | 24px 高、12px 字号、`--bg` 底、底部 1px 线；名称 `--text` + hover 下划线；超长 clip |

## 三、左侧列表（树形虚拟滚动）

| # | 功能 | 状态 | 备注 |
|---|------|------|------|
| L1 | 根行（depth 0，默认展开，可折叠；折叠=隐藏整棵子树） | ✅ | 与 HTML `buildRows` 同构 |
| L2 | ".." 上级行（depth 1，数据列留空） | ✅ | 单击选中、双击/回车进入父目录 |
| L3 | 扁平化行模型 + 虚拟滚动 | ✅ | `first = floor(scroll/30)-2`，`count = ceil(h/30)+4` |
| L4 | 树形连接线（竖线 + ├/└ 拐角 9px、物理 1px、上下各溢出 1px） | ✅ | `treeLines`/`moreAfter` 逐行照搬；线色 `--tree-line` |
| L5 | 箭头点击展开/折叠（文件行留空占位） | ✅ | ▸/▾ 字符（15px，居中于 22px 箭头列），文件行不画 |
| L6 | 单击选中 + Ctrl/Shift 多选（含 Shift 范围、锚点语义） | ✅ | 与 HTML `selectRow` 一致；Shift 无锚点时退化为普通点击 |
| L7 | 双击目录进入 / 文件打开 | ✅ | 空目录也能进（HTML 同） |
| L8 | 表头排序（5 列，首次点击=名称升序/其余降序） | ✅ | 比较键与方向同 HTML；名称用 `_wcsicmp`（见第八节） |
| L9 | 列宽拖拽（手柄 ±3px，最小宽 120/56/48/56/56，持久化） | ✅ | 落盘 `layout.json`（HTML 用 localStorage `dm-cols`） |
| L10 | 左右分栏拖拽（0.15–0.85，持久化） | ✅ | 5px 分栏条 + 1px 线，hover 变 accent |
| L11 | 虚拟滚动只画可视行 | ✅ | ImGui 自绘行 |
| L12 | 吸顶行（展开目录滚出视野时钉在列表顶部） | ✅ | `updatePinRow` 算法照搬；面板底色 + 上下边线。**命中测试优先落在吸顶行**（盖住可视区第一行）：箭头点击=收起/展开该浮动目录、单击=选中、双击=进入；窗口刚被点活那一帧用矩形兜底，避免「第一次点击」被吞 |
| L13 | 列表选中 → 树图高亮联动 | ✅ | 选中集合变化即重绘焦点框 |
| L14 | 树图点击 → 列表定位（展开祖先链 + 滚到约 1/3 处） | ✅ | `LocateInList()` = HTML `locateInList` + `scrollToNode` |
| L15 | 键盘导航（↑↓←→/Enter/Backspace） | ✅ | ←→ 只展开/折叠+跳父行（不进入），与 HTML 一致 |
| L16 | F2 重命名 / Delete / Shift+Delete / Ctrl+C·X·V / Ctrl+Shift+N | ✅ | ImGui 版**真做**了（HTML 只 post `cmd:"shell"`，而宿主没有该分支 → 实际是空操作，见第八节） |
| L17 | 右键菜单作用于**选择集**、右键不改变选择 | ✅ | 与 HTML `document.contextmenu` 语义一致 |
| L18 | 状态栏 st0/st1/st2 | ✅ | st1=`条目 N`、st2=`总大小 X`，N 含根行与 `..` 行 |
| L19 | 进度条动画 + 实时文字（项/字节/速度） | ✅ | 状态栏右侧 110×5 胶囊 + 1.1s ease-in-out 循环滑动 + `N 项 · S · N/s 项/s · S/s` |
| L20 | 空状态提示 | ✅ | `还没有数据 — 输入路径后点「扫描」` |
| L21 | 进入目录淡入动画 | ✅ | 220ms easeOutExpo：列表整帧 alpha + 树图缩放 0.965→1（见第八节①） |

## 四、右侧树图

| # | 功能 | 状态 | 备注 |
|---|------|------|------|
| M1 | squarify 递归布局（worst 判据 + 短边切行 + 剩余区域递归） | ✅ | **数值比对通过**（第九节） |
| M2 | 4× 超采样虚拟画布 + 视口映射（k=0.25） | ✅ | `LAYOUT_SCALE=4`，与 HTML 完全一致 |
| M3 | 面积权重 wOf：g × 顺序（含 _G/_Gsum/兜底为 1） | ✅ | 含 `id` 快路径、`log2(x+2)` 的 +2、非有限值兜底 |
| M4 | 对角渐变（片元着色器 → 四角顶点色）+ 1px 边缘浮雕 | ✅ | TL=+0.22/+0.16、BR=−0.15/−0.11、上/左 +0.06、下/右 −0.05；浮雕只对 ≥10px 块烘焙 |
| M5 | 目录外框（仅最外层深度，1px，rgba(190,196,206,.55)，<8×8 跳过） | ✅ | |
| M6 | 大块文字（名称 + 大小两行、clip + 省略号、12/11px 两档） | ✅ | 阈值 `w≥34&&h≥18`；大小行 `h≥34&&w≥110`；`fitText` 二分截断 + `…` |
| M7 | 悬停/选中焦点框 + 信息面板（一体，贴框外侧翻边） | ✅ | 4 行：完整路径 / 名称 / `大小 · 占比%` / `目录 · N 项`或`文件`；1 物理 px 白框（hover .85 / 选中 .95，**不填充**） |
| M8 | 点击选中 + 列表联动 | ✅ | 只改焦点项（不改多选集合），与 HTML 一致 |
| M9 | 双击进入（目录）/ 打开（文件） | ✅ | |
| M10 | 滚轮父级框选（并集框 + 四周压暗 35% + 白框 + 会话基块 + 500ms 防误触） | ✅ | 含 `resetWheelFocus`（鼠标换块即重置会话）、框选时抑制悬停框 |
| M11 | 扩展名配色（10 分类 + 兜底色，可增删改查） | ✅ | 默认表与 HTML 一致（含 `#5a6472` 兜底）；落盘 `extmap.json` |
| M12 | 主题联动（深/浅底色与配色） | ✅ | 底色 #2b313b/#e8ecf1，目录基色 #3a4250/#c8cfd8 |
| M13 | 布局缓存（当前目录/尺寸/主题/设置/配色指纹） | ✅ | 且**每次重烘焙清空 hover**（对齐 HTML `bakeView` 里 `tmHoverId=null`） |
| M14 | 渐进细化（BFS 后台拉子项） | ➖ | 后端单进程全量树，无需懒加载通道 |
| M15 | 渐进布局（caps 分轮 + 新层淡入） | ➖ | HTML 线上路径也已是「一次全量布局」（`startProgressiveLayout` 是死代码） |
| M16 | 背景底色（当前目录） | ✅ | |
| — | 设置项 `maxDepth/minSizeMB/maxRects/kidsCap/showLabels/colorScheme` | ⚠️ | **在 HTML 里这些对树图不生效**（`collectLeaves` 只认 0.02px/12px 阈值与 cap=999，配色只认 extMap）。为保证两侧出图一致，ImGui 侧同样不生效；设置项仍在面板里（与 HTML 面板一致） |

## 五、设置面板

| # | 功能 | 状态 | 备注 |
|---|------|------|------|
| S1 | 设置模态（遮罩 rgba(0,0,0,.5) / ✕ / 保存 / 点遮罩关闭 / 无 Esc） | ✅ | 单窗口实现：窗口铺满视口、`WindowBg=rgba(0,0,0,.5)` 当遮罩 |
| S2 | 扫描组：threads / skipHidden / followReparse | ✅ | 标签列固定 132px、数字框 64px |
| S3 | 树图组：maxDepth / minSizeMB / maxRects / kidsCap / lazyDepth / showLabels / colorScheme | ✅ | `0=∞` 徽标只对 inf 字段显示；其余 hint 走 tooltip（HTML 用 title） |
| S4 | 界面组：defaultSort / rowExtraPx / confirmDelete | ✅ | |
| S5 | 文件颜色卡片（分组名 → 颜色 → 后缀串 → ✕ 删除；其他兜底色；新分类） | ✅ | 改动**即时生效**并落盘 |
| S6 | 右键菜单显隐（18 项 checkbox，多列密集排） | ✅ | 标签与 HTML 一致 |
| S7 | 保存即生效（写 config.json + 立即重渲染树图） | ✅ | 底部提示「已保存（扫描类设置下次扫描生效）」停留 700ms 后关闭 |

## 六、消息/后端通道

| # | 功能 | 状态 | 备注 |
|---|------|------|------|
| H1 | 扫描结果树（全量） | ✅ | `ScanEngine` 回调 → 加锁投递 → **UI 线程**接管（避免跨线程碰 UI 状态） |
| H2 | 进度回调 | ✅ | 原子变量 + 帧内差分算速度 |
| H3 | 扫描失败复位 | ✅ | 无效路径直接拒绝并提示（对齐宿主 `scanfail`） |
| H4 | 打开文件（ShellExecute open） | ✅ | |
| H5 | shell 动作（重命名/删除/新建/复制/剪切/粘贴） | ✅ | `SHFileOperation`/`MoveFileW`/`CreateDirectoryW`；粘贴后按子树重扫刷新模型 |
| H6 | 浏览目录对话框 | ✅ | |
| H7 | 预字库/二进制传输/懒加载 | ➖ | 单进程全量树 |
| H8 | 设置持久化（config.json） | ✅ | 另加 `layout.json`（列宽/分栏）与 `extmap.json`（配色） |
| H9 | 系统壳右键菜单（IShellFolder + IContextMenu，多选合并） | ✅ | 独立 MTA 线程 + 隐藏 owner 窗口 + TrackPopupMenu（与 `webview_host.cpp` 同款；实测 COM 全链路跑通） |

## 七、验收口径（用户点名的硬要求）

| # | 要求 | 状态 | 证据 |
|---|------|------|------|
| 1 | 箭头与文字严格同一行、同列对齐 | ✅ | 行内所有列绝对定位：名称 `ARROW + depth*INDENT`、数据列 `colX[k]`，与 HTML 同一套公式（截图核对） |
| 2 | 树图有渐变、层次分明 | ✅ | 对角渐变 + 目录外框 + 主题双色板 |
| 3 | 双击进入目录、有返回按钮（返回/上级/面包屑） | ✅ | 截图核对 |
| 4 | 列表滚动不丢行、初次刷新立即可见 | ✅ | 虚拟滚动窗口 ±2 行冗余 |
| 5 | MFT 按钮双向绑定（管理员=直读 / 非管理员=普通遍历 / 点击提权 / 失败回退） | ✅ | |
| 6 | 进目录/出图有过渡动画 | ✅ | 220ms 淡入 + 树图缩放（`--frames` 截图可见过渡后的稳定态） |
| 7 | 左侧列表：吸顶行、展开箭头可用、可多选、键盘可用 | ✅ | 吸顶行截图核对 |

## 八、与 HTML 有意保留的差异

1. **过渡动画的列表部分只用淡入**：HTML 对列表做 `transform:scale(.965→1)` + 淡入；
   ImGui 没有窗口级仿射变换，列表用整帧 alpha 淡入、树图用缩放 + 淡入（观感等价）。
2. **名称排序用 `_wcsicmp`**：HTML 是 `localeCompare(...,"zh-CN")`（ICU 拼音序）；
   ImGui 用系统 API 的序数比较，非 ASCII 名称的相对顺序可能与 HTML 略有不同。
3. **信息面板第 4 行「目录 · N 项」的 N**：HTML 用后端 `dirCount`（子树目录数）；
   ImGui 的 `ScanNode` 没有该字段，改用**直接子目录数**（O(子项)，避免每帧遍历子树）。
4. **文件操作是真实现**：HTML 的 F2/Ctrl+C·X·V/Ctrl+Shift+N 会 post `cmd:"shell"`，
   但 `webview_host.cpp` 里**没有** `cmd=="shell"` 分支 → 在 Web 版里这些键是空操作。
   ImGui 版按语法意图真做了（重命名/删除/复制/剪切/粘贴/新建文件夹），属超集。
5. **调试/自检开关**：`--hoverpos=x,y`、`--scroll=N`、`--click=x,y`、`--wheel=N`、
   `--key=F2`、`--opensettings`、`--ctxmenu=<path>`、`--saveprefs`、`--shot=<file.bmp>`、
   `--frames=N`。这些只用于自动化自检，不影响正常使用。
6. ~~界面偏好存放位置~~ —— **已统一**：原先 ImGui 版把配色写 `extmap.json`、列宽写
   `layout.json`，与 Web 版的 `ui-prefs.json` 各存一份；现在 ImGui 版按 Web 的同一
   schema 读写 `ui-prefs.json` + `diskmate.ini`（见第十节），两个前端的主题、面积算法、
   扩展名配色、列宽分栏、上次路径、MFT 模式全部共用。

## 九、验证方法与证据（本轮实测）

### 1) 布局算法数值比对（最强证据）

把 `web/index.html` 的 `wOf / sizedItems / squarify / collectLeaves` **原样搬到 Node**
（`temp/verify_html_layout.mjs`），对同一棵扫描树、同一 4× 画布尺寸跑一遍，与 C++ 侧
`TMBAKE` 日志逐项对比：

```
树：D:\Desktop\diskmate\dmtest（1313 项 / 177 MB）   画布 4×：2248 × 2572
                      C++ (ImGui)          HTML (JS)
allLeaves             1260                 1260
allDirs                 52                   52
HUGE.bin px²       1,599,295            1,599,295
d_50MB.bin         1,494,935            1,494,936   (±1 取整)
c_1MB.bin            211,415              211,416   (±1)
root_file.bin        187,215              187,216   (±1)
deep_file_1.bin       82,780               82,780
```

复现：

```powershell
build_web\dumpjson.exe temp\tree2.json D:\Desktop\diskmate\dmtest
build\diskmate_imgui.exe --elevated=1 --autoscan=D:\Desktop\diskmate\dmtest `
    --shot=temp\a.bmp --frames=45      # 看 build\diskmate_imgui.log 里的 TMBAKE 行
node temp\verify_html_layout.mjs 2248 2572 D:/Desktop/diskmate/temp/tree2.json
```

### 2) 界面截图自检

`diskmate_imgui.exe --shot=<file.bmp> --frames=N` 会把后缓冲写成 24bit BMP（无需人操作），
配合注入开关可覆盖各状态：

| 场景 | 命令要点 | 结果 |
|---|---|---|
| 主界面（深色） | `--autoscan=<dir> --frames=45` | 工具栏/面包屑/列表/树图/状态栏齐全 |
| 大目录（16 万矩形） | `--autoscan=D:\Desktop` | `TMBAKE … cover=1.0000`、色块包围盒 = 面板矩形、`DRAWDATA` 分块命令 |
| 悬停焦点框 + 信息面板 | `--hoverpos=900,300` | 4 行信息面板 + 1px 白框（像素级核对到近白像素） |
| 吸顶行 | `--scroll=330` | 首行钉住根目录行（面板底色 + 上下边线） |
| 点击→列表联动 | `--click=900,300` | 展开祖先链 + 滚到该行 + 面板显示完整路径 |
| 滚轮父级框选 | `--click=780,250 --wheel=1` | 压暗框外 + 白框 + `目录 · N 项` 信息标签 |
| 设置模态 | `--opensettings` | 遮罩 50% 压暗（像素核对 30,34,40 → 15,17,20）+ 4 卡片 + 18 项菜单网格 |
| 扫描进度 | `--autoscan=C:\ --frames=30` | 按钮变红「停止」+ 胶囊进度条 + `70,471 项 · 41.78 GB · 115,336 项/s · 28.48 GB/s` |
| 浅色主题 | `config.json` 改 `theme:1` | 全链路浅色（工具栏/列表/树图/面板） |
| 进入目录 / `..` 行 | `--click=200,144 --key=Enter` | 面包屑 `… › icon`、`..` 行、根行占比 249.3%（HTML 同款口径） |
| 重命名 / 新建文件夹 | `--click=… --key=F2` / `--key=CtrlN` | 模态对话框出现、重命名预填选中文件名 |
| 系统壳菜单 | `--ctxmenu=<path>` | 日志 `shellmenu paths=1 invoked=0`：COM 全链路 + 弹出等待（取消返回 0） |

### 3) 构建

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j            # 7 个目标全绿（含 4 个 C++ 前端/工具）
```

## 十、单后端 / 多前端：共用配置文件与设置

三个前端（`diskmate_web.exe` / `diskmate_imgui.exe` / `diskmate_native.exe`）**同目录部署**
（`release\DiskMate_v1.0\`），共用以下持久化文件：

| 文件 | 归属 | 内容 | 谁读写 |
|---|---|---|---|
| `config.json` | settings.cpp（`AppSettings`） | 扫描 3 项、树图 6 项、界面 3 项、18 项右键菜单开关、lastPath | 三个前端都读写；设置面板「保存」写它 |
| `ui-prefs.json` | **Web 页面**（宿主当不透明 blob 存取，UTF-8 无 BOM） | `theme` / `mapKind` / `ordKind` / `powAlpha` / `extMap` / `extFallback` / `cols`（列宽+分栏） | Web 读写；**ImGui 按同一 schema 读写**（`src_imgui/shared_prefs.cpp`） |
| `diskmate.ini` | Web 宿主（key=value） | `lastPath` / `mft` | Web 读写；**ImGui 读写同键** |

规则（`shared_prefs.cpp` 顶部注释同）：

* **四个标量**（theme / mapKind / ordKind / powAlpha）两边都有位置：
  **读以 `ui-prefs.json` 为准**（Web 版的真源），缺失时保留 `config.json` 的值；
  **写则两个文件都写**，于是原生 Win32 前端读 `config.json` 也保持一致。
* **不认识的键原样保留**：ImGui 保存时只改自己拥有的键，Web 侧新增的键（例如用户
  自己加的配色分组键、将来新增的偏好）不会被抹掉——实测注入 `webOnlyKey`/`themeNote`
  后保存，两个键原样还在。
* **扩展名配色的键名与 Web 一致**（`img`/`vid`/…，自定义组用中文名当键，如 `系统`）；
  ImGui 侧的显示名 = 短键 → 中文表（与页面 `EXT_GROUP_NAMES` 同一张表），未知键直接显示键名。
* `cols` 是**双向**的：Web 侧 `saveColPrefs()` 现在也把它写进 blob，`applyPrefs()` 会应用；
  ImGui 侧拖列宽/分栏同样写它 —— 两个前端的列宽与分栏比例一致。

实测（把 release 目录的 `config.json` + `ui-prefs.json` + `diskmate.ini` 拷到临时目录，
用 Web 版写出的文件启动 ImGui 版）：

```
日志：ui-prefs loaded theme=0 mapKind=0 ordKind=0 extGroups=7
      StartScan root=… mode=MFT            ← mft=1 来自共用的 diskmate.ini
保存后 ui-prefs.json：theme/mapKind/ordKind/powAlpha 原值 + 新增 cols +
                      用户的「系统」配色分组 + 未知键原样保留
保存后 config.json：mapKind=0 ordKind=0 powAlpha=0.4 theme=0（与 ui-prefs 收敛一致）
点击工具栏「浅色」→ ui-prefs.json theme 变 "light"，config.json theme 变 1
```

## 十一、DPI 适配与大网格顶点提交（两个实测踩坑）

### 1) 高 DPI（本机实测 150%）：坐标空间保持「逻辑像素」

**症状**：150% 缩放下字体又小又糊（比 Web 版小一圈）。

**原因**：窗口/后缓冲是物理像素，而布局与字号按物理像素写死 → 13px 字形只有
Web 版（13 CSS px × dpr=1.5 ≈ 19.5 物理 px）的 2/3 大，小字号在 150% 屏上显得糊。

**做法**（ImGui 1.92+ 的正解，不用手改任何几何常量）：

```
每帧（ImGui_ImplWin32_NewFrame 之后、ImGui::NewFrame 之前，见 ApplyDpiToImGui）：
  io.DisplaySize             = 物理客户区 / uiScale      // ImGui 坐标空间 = 逻辑像素 = CSS px
  io.DisplayFramebufferScale = (uiScale, uiScale)        // 后端 viewport/scissor 放大到物理像素，
                                                         // 同时 ImGui 拿它当字体 RasterizerDensity
  io.AddMousePosEvent(物理鼠标 / uiScale)                 // 鼠标换算到逻辑空间（delta 也随之正确）
字体：按【逻辑】13px 载入（AddFontFromFileTTF(..., 13.0f)），不要手动放大字号
```

* ImGui 1.92 的字体系统会按 `RasterizerDensity` **按需栅格化**字形到物理尺寸 → 清晰；
  显式放大字号或设 `FontGlobalScale` 反而会双重缩放（1.92 把它挪成了 `style.FontScaleMain`）。
* 因为坐标空间是逻辑像素，**树图的 12px / 0.02px / 34×18 等阈值天然就是 CSS px 语义**，
  与 `web/index.html` 完全一致 —— 不需要按 DPI 换算阈值。
* 细线用 `HAIR = 1/uiScale`（对应 HTML 的 `--hair = 1px/dpr`），保证物理 1px 不发虚：
  列表层级线、树图焦点框、目录外框都用它。
* `WM_DPICHANGED`：换显示器时重算 `g_uiScale` → 重载字体 → 重建样式 → 失效树图布局。
* 初始窗口尺寸 `1280×800 × uiScale`（与 Web 宿主同款 `MulDiv(1280, dpi, 96)`）。

实测日志：`DPI system=144 window=144 uiScale=150%`；`DRAWDATA display=1265x762`（= 物理
1898×1144 ÷ 1.5）。

### 2) 大网格顶点提交：ImDrawIdx 是 **16 位**

**症状**：扫大目录（如 `D:\Desktop`，16 万个矩形）后，树图只画在面板左上角一小块
（实测只覆盖 12.9% 面积），其余空白。

**原因**：1.92/1.93 的 `ImDrawIdx` 恒为 `unsigned short`，
`IMGUI_USE_32_BIT_INDEX` **在这个版本里根本不存在**（CMake 里那个定义是空操作）。
一次性 `PrimReserve(98 万索引, 66 万顶点)` 会触发 ImGui 的 VtxOffset 分段：
`_CmdHeader.VtxOffset = VtxBuffer.Size`，而后端会把它当 **BaseVertex** 加在索引上
（日志实证：`cmd elem=998700 vtxOfs=4`）。手工写入的“绝对索引”被整体 +4，
再加上 16 位回绕 → 整片色块错位。

**做法**：按 **≤16000 个四边形（64000 顶点）** 分块提交（`treemap_panel.cpp` 的提交循环）：

```
for (q = 0; q < quads; q += 16000) {
    dl->PrimReserve(n*6, n*4);
    base = dl->_VtxCurrentIdx;          // 本块索引基准（PrimReserve 内部切 VtxOffset 时会归零）
    ... 写入顶点（动画时逐顶点做缩放/淡入）...
    iw[i] = base + 块内 0/1/2 索引;      // 索引一律相对本块
    dl->_VtxCurrentIdx += n*4;
}
```

实测日志（每块 96000 索引 = 16000 四边形，VtxOffset 每块 +64000）：
`cmd elem=96000 clip=700,76-1265,737 vtxOfs=0 / 64004 / 128004 …`
修后**色块铺满率 1.0000**，色块包围盒 = 面板矩形（物理 847×991）。

### 2) 后缓冲必须与客户区**完全一致**（画面发糊 + 鼠标错位的真凶）

**症状**：屏幕上字发糊、鼠标点哪儿和看到的不一致（越靠边越明显）。
但 `--shot` 抓出来的后缓冲截图**完全正常**（这正是我先漏掉它的原因）。

**原因**：`diskmate_imgui` 的窗口初始「外框」尺寸是 `1280×800 × uiScale`（150% → 1920×1200），
客户区其实只有 **1898×1144**。而第一次 `WM_SIZE` 发生在 `CreateWindowEx` 期间
（那时 D3D 还没建），被 `if (g_swap)` 挡掉了，之后再没有尺寸变化 →
**交换链一直是 1920×1200**。DXGI 把 1920×1200 的缓冲铺到 1898×1144 的客户区时
按横纵**不同比例**缩放（×0.988 / ×0.953）→
* 整幅画面被非等比重采样 → 发糊；
* 绘制坐标与客户区鼠标坐标按不同比例对应 → 越靠屏幕下方偏得越多（底部约 54px）。

**做法**：`EnsureBackBufferSize()`（每帧兜底）——`GetClientRect` 与后缓冲尺寸不一致就
`ResizeBuffers` + 重建 RTV。实测日志 `backbuffer resized to 1898x1144`。

**验证**（物理点 → 逻辑坐标 → 命中行，全部用自检开关跑）：

```
--click=300,220  →  日志 LISTCLICK phys=300,220 logical=200,146 row=1 node=超分
                     截图里白色选中描边落在 y=199..241（期望行1 = 逻辑 132..162 × 1.5）
字形锐利度：笔画上升沿过渡 1~2 px（原生 ~19.5px 栅格化；若被 1.5× 放大过会是 3~4 px）
```

### 3) 滚轮框选的「暗框」要及时作废

滚轮在树图上滚 = **逐级父级框选**（Web 版同款、用户点名要求的功能）：框外压暗 35% +
白色框 + 信息标签。它的并集框是按**当时的布局坐标**算出来的，所以**布局一变（进目录 /
重扫 / 换主题 / 改列宽分栏）就必须作废**，否则会留下一个位置和大小都不对的暗框
（看起来就是「树图突然变暗，像多了个滑窗」）。HTML 在 `enter()` 里清空滚轮会话，
ImGui 侧现在同样在 `Enter()`、扫描完成、以及每次重新布局时调用 `TreemapClearWheel()`。

### 4) 1px 细线必须吸附到物理像素

**症状**：树图的「第一层级目录外框」在屏幕上看着像没画（但实际画了 1 万多个像素）。

**原因**：细线宽 `HAIR = 1/uiScale`（150% 下 = 1 物理像素），但线的位置是
`roundf(逻辑坐标)`。逻辑整数 × 1.5 得到的是 `.0` 或 `.5` 物理像素：
落在 `.5` 上的那条线只被 GPU 覆盖一半，再叠加 `alpha=140` → 实际强度只剩约 1/4，肉眼看不见。

**做法**：`SnapPx(v) = roundf(v * g_uiScale) / g_uiScale`（dm.h），把线的最终屏幕坐标
吸附到物理像素边界。实测同一批外框在修复前后：修复后 `x=1548` 处的像素为
`(147,201,195)`，与块色 `(128,240,215)` 形成清晰的一条 1px 浅灰线（与 HTML 的
`rgba(190,196,206,.55)` 完全一致）。

### 5) 顶部工具栏的宽度预算

150% DPI 下窗口逻辑宽度只有 1265（1920 物理），旧写法把路径框写死 `≥120`，
尾部按钮（主题/MFT/设置）就被挤出可视区。现在：
① 先按完整标签算，放不下就换短标签（`← ↑ … MFT 遍历 ⚙`）并去掉「路径」标题；
② 尾部组**右对齐**；
③ 仍然放不下（很窄的窗口）→ 尾部控件换到**第二行**（`g_toolbarH` 加高，面包屑/面板随之下移）。

### 6) 弹出层期间必须挡住「自算矩形」的命中

树图的 `inside` 是自己按矩形算的（不走 ImGui 窗口命中），所以下拉菜单展开时，
点下拉项会**穿透**到下面的树图上并改选中/焦点。现在统一用
`g_showSettings || g_renameOpen || g_newFolderOpen || IsPopupOpen(AnyPopupId|AnyPopupLevel)`
把 `inside` 置假（列表的 `AppFocusLost` 兜底同样加了这一条）。
自检：同一个坐标，不点开下拉时 `TMCLICK node=…`，点开下拉后**没有** TMCLICK。

### 7) 导航要同步路径框

`Enter()`（双击列表行/双击树图块/返回/上级/面包屑）现在会把路径框与
`g_st.lastPath` 同步成当前目录 —— 否则进入子目录后点「扫描」扫的还是原来那个父路径
（用户报「不能扫描子路径」）。实测日志：`ENTER D:\Desktop\diskmate\icon pathBuf=D:\Desktop\diskmate\icon`。

## 十二、代码结构（ImGui 前端）

```
src_imgui/
├─ dm.h                公共头：几何常量 / 配色 / 全局状态 / 跨文件函数声明
├─ main_imgui.cpp      入口、D3D11、状态与模型、工具栏、面包屑、列表、状态栏、
│                      键盘、扫描调度、截图自检
├─ treemap_panel.cpp   树图：权重 / squarify / collectLeaves / 顶点烘焙 / 焦点框 /
│                      信息面板 / 滚轮框选 / 命中测试
├─ settings_modal.cpp  设置模态 + 重命名 / 新建文件夹对话框
├─ shell_menu.cpp      系统壳右键菜单（独立 MTA 线程 + 隐藏 owner）+ 自绘兜底菜单
└─ shared_prefs.cpp    多前端共用偏好：ui-prefs.json（UTF-8，保留未知键）+ diskmate.ini
```

构建目标：`diskmate_imgui`（`app.rc` 提供图标与 DPI manifest；链接
`d3d11 dxgi d3dcompiler d2d1 dwmapi shell32 gdi32 user32 ole32 oleaut32 shlwapi comdlg32`）。
