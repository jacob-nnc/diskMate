# DiskMate

> 对标 [WizTree](https://diskanalyzer.com/) 的 Windows 磁盘空间分析器。NTFS `$MFT` 直读，
> 百万级文件的矩形树图（Treemap）可视化。C++17 + Dear ImGui / WebView2，**无第三方运行时依赖**。

```
┌──────────────────────────────────────────────────────────────┐
│ ← 返回  ↑ 上级  路径 [D:\              ] 浏览 扫描  浅色 ⚙    │
├───────────────────────────┬──────────────────────────────────┤
│ 名称          大小   占比 │  ████  ██  █  █  ██  ███  █      │
│ ▾ build      2.1 GB  38%  │  ███████  ████  ██  █  ████  █    │
│   ▸ obj       800 MB 14%  │  ██  ████  ██████  ███  █        │
│   file.bin    1.2 GB 22%  │      (面积 = 文件/目录大小)       │
├───────────────────────────┴──────────────────────────────────┤
│ 扫描完成 · MFT 直读 · 1,242,442 项 · 3.4 TB · 用时 4.2s       │
└──────────────────────────────────────────────────────────────┘
```

## 特性

| | 说明 |
|---|---|
| **两种扫描模式** | **MFT 直读**：直接解析 NTFS 卷 `$MFT`，全盘百万文件秒级完成（需管理员）；**普通遍历**：兼容任意路径/文件系统，非管理员可用，失败自动回退 |
| **三套前端** | `diskmate_imgui`（Dear ImGui + D3D11，单进程、体积小、启动快）；`diskmate_web`（WebView2 + HTML/CSS/WebGL，界面最好改）；`diskmate_native`（纯 Win32 + D2D，兼容性最好） |
| **矩形树图** | squarify 布局，4× 超采样虚拟画布；WebGL 实例化 / D3D11 顶点缓冲两套渲染；色块渐变 + 1px 浮雕 + 目录外框 |
| **面积算法可换** | `g(x)`：`x` / `log₂x` / `log₂²x` / `√x` / `x^α`；顺序：先和无序再 `g`，或先 `g` 再求和 |
| **左侧树** | 列宽可拖、点表头排序、层级线、吸顶父级行、多选（Ctrl/Shift）、面包屑、前进后退 |
| **交互** | 悬停高亮 + 信息标签、选中联动定位、双击进入目录、**滚轮逐级父级框选**、右键资源管理器同款菜单（原生 shell 菜单） |
| **文件操作** | F2 重命名、Ctrl+C/X/V、Ctrl+Shift+N 新建文件夹、Delete 删除（走资源管理器通道，支持回收站） |
| **共用配置** | 三个前端共享同一份 `config.json` / `ui-prefs.json` / `diskmate.ini`，换前端不丢设置 |

## 构建

### 依赖

| | 要求 |
|---|---|
| 编译器 | **MinGW-w64 GCC 13+**（本项目用 15.2 验证）或 MSVC 2022 |
| 构建 | CMake 3.20+ |
| 可选 | **WebView2 SDK**（只有 `diskmate_web` 需要，缺了会自动跳过该目标） |

ImGui 已随仓库附带（`third_party/imgui`），**不需要额外下载**。

### MinGW（推荐，产物最小最快）

```powershell
git clone <你的仓库地址> diskmate
cd diskmate
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 8
# 产物：build\diskmate_imgui.exe / diskmate_native.exe / diskmate_web.exe
```

或者直接用附带的脚本（会检查依赖并告诉你缺什么）：

```powershell
.\build.ps1            # 全部目标
.\build.ps1 -Target diskmate_imgui
```

### 只构建不需要 WebView2 的部分

```powershell
# 随便给一个不存在的路径 → CMake 会打印「跳过 diskmate_web」并继续
$env:DISKMATE_WEBVIEW2_DIR = "C:\nonexistent"
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
```

### 需要 WebView2 前端时

```powershell
nuget install Microsoft.Web.WebView2 -Version 1.0.2210.55 -OutputDirectory C:\sdk
$env:DISKMATE_WEBVIEW2_DIR = "C:\sdk\Microsoft.Web.WebView2.1.0.2210.55\build\native"
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 8
```

> `WebView2Loader.dll` 会在构建后自动拷到 exe 同目录；发布时要一起带上。

## 使用

1. 双击 `diskmate_imgui.exe`（或 `diskmate_web.exe`）。
2. **首次运行会弹 UAC**：同意 = 可用 MFT 直读（快几十倍）；拒绝 = 普通遍历，功能一样只是慢。
3. 输入/浏览选择要扫描的目录。**盘符根**（`C:\`、`D:\`）才能用 MFT 直读 —— 子目录会自动退化为普通遍历。
4. 左侧点表头排序、拖列宽；**双击目录**进入；右侧树图 **滚轮**逐级框选父目录，下滚滚回。
5. 右键 = 资源管理器菜单；F2 重命名，Delete 删除，Ctrl+C/X/V 复制粘贴（支持回收站）。

### 配置文件

| 文件 | 内容 | 谁在写 |
|---|---|---|
| `config.json` | 扫描/树图/界面全部设置 | 三个前端共用 |
| `ui-prefs.json` | 主题、面积算法、扩展名配色、列宽 | 三个前端共用（Web 版同款键名） |
| `diskmate.ini` | `lastPath` / `mft` | 三个前端共用 |

### 首次运行被 SmartScreen 拦下？

程序**没有代码签名**时，Windows 会弹
「Microsoft Defender SmartScreen 阻止了无法识别的应用启动 / 发布者未知」。
这不是报毒：

* 点「**更多信息**」→「**仍要运行**」即可（每个新版本的 exe 需要点一次）；
* 少弹框：Windows 安全中心 → 应用和浏览器控制 → 基于声誉的保护 → 「检查应用和文件」改成「警告」；
* 根治：**给 exe 代码签名**（见下）。

## 代码签名

```powershell
# 用 .pfx 证书签名全部 exe（SHA256 + RFC3161 时间戳）
.\tools\sign.ps1 -Pfx C:\cert\codesign.pfx -Password (Read-Host -AsSecureString) -Path D:\DiskMate_v1.0

# 或者用证书库里已安装的证书（按指纹）
.\tools\sign.ps1 -Thumbprint 0123456789ABCDEF... -Path D:\DiskMate_v1.0

# 只看当前签名状态
.\tools\sign.ps1 -Verify -Path D:\DiskMate_v1.0
```

* 签名需要一张**代码签名证书**（CA 签发的 OV/EV 证书，或 Azure Trusted Signing）。
  自签名证书只能本机测试，**不会**消除 SmartScreen 提示（`-SelfTestCert` 可以把流程跑通）。
* **先签名、后打包**：只要重新编译，exe 的哈希就变了，旧签名立刻失效，所以顺序是
  `build → sign → zip`。
* `WebView2Loader.dll` 是微软签名的，不用（也不能）重签。

## 项目结构

```
src/                 扫描引擎 + 原生 Win32 前端 + WebView2 宿主
  scanner_M.cpp        NTFS $MFT 解析（只读打开卷）
  scanner.cpp          普通遍历（多线程）
  treemap.cpp          原生版树图（D2D）
  webview_host.cpp     WebView2 宿主 + JS 桥 + 二进制树传输
  main.cpp             原生 Win32 前端
src_imgui/           Dear ImGui 前端（D3D11）
  treemap_panel.cpp    树图：布局 + 烘焙 + 命中 + 滚轮框选
  main_imgui.cpp       工具栏/列表/面包屑/状态栏/键盘/自检钩子
  settings_modal.cpp   设置面板
  shell_menu.cpp       原生 shell 右键菜单
web/                 WebView2 前端（index.html：布局/渲染/交互全在里面）
third_party/imgui/   Dear ImGui（随仓库附带，见 LICENSE.txt）
docs/                设计与验收文档（含与 Web 版的逐项对齐核对）
installer/           Inno Setup 打包脚本
tools/               图标/测试数据生成脚本、布局与命中的校验脚本
```

## 测试与自检

```powershell
# 树图布局：和 web/index.html 的 JS 版逐项对数字（叶子/目录数、各块 px²）
node tools\verify_html_layout.mjs 848 996 dump.json

# 树图命中：把 index.html 里的函数抽出来跑，逐点比对「递归下行」与「线性扫描」
node tools\verify_hit_test.mjs
```

ImGui 前端内置自检钩子（不需要手动点界面）：

```powershell
.\diskmate_imgui.exe --elevated=1 --autoscan=D:\ --frames=90 --shot=out.bmp
.\diskmate_imgui.exe --hoverpos=1400,500      # 模拟悬停（物理像素）
.\diskmate_imgui.exe --click=300,220          # 模拟点击
.\diskmate_imgui.exe --dblclick=300,220       # 模拟双击
.\diskmate_imgui.exe --key=F2                 # 模拟按键
.\diskmate_imgui.exe --opensettings           # 直接打开设置面板
```

运行时会在 exe 同目录写 `diskmate_imgui.log`（DPI、扫描、树图烘焙、绘制命令等诊断）。

## 性能参考

| 场景 | 规模 | 结果 |
|---|---|---|
| MFT 直读 `C:\` | 124 万项 / 3.4 TB | 约 4 s（取决于磁盘） |
| 树图布局（JS 版） | 84.6 万块 | 布局 857 ms + 烘焙 752 ms |
| 树图命中（ImGui/递归下行） | 3.3 万块实测 | 单次约 0.5 µs；旧线性扫描 61 ms/2000 次 → 新实现 1.1 ms/2000 次（**55×**）|

## 已知限制

* 树图的 6 个老设置（最大深度 / 最小尺寸 / 最大矩形数 / 子项上限 / 显示标签 / 配色方案）在
  **Web 版和 ImGui 版里都是"存而不用"**（历史遗留旋钮），实际生效的是面积算法 + 顺序 + α。
* ImGui 版的列表进入动画只做透明度渐入（没有 HTML 那种缩放）；排序用 `_wcsicmp` 而非 ICU collation。
* Web 版的树图布局在 JS 里做（`layoutAll`）；把布局搬到 C++ 后端是下一步优化方向。
* 多显示器不同 DPI 下切换显示器会重载字体，极短瞬间可能闪一下。

## 许可

见 `LICENSE`（未定则默认保留所有权利；如需开源请在此处改为 MIT/Apache-2.0 等）。

`third_party/imgui` 为 Dear ImGui，MIT 许可，见 `third_party/imgui/LICENSE.txt`。
#   d i s k M a t e  
 