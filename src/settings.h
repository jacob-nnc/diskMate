#pragma once

#include <windows.h>

#include <string>
#include <vector>

// ================= 右键菜单配置（数据驱动，JSON 双向绑定） =================
// 每一项对应一个可显示/隐藏的命令；shellMenu 控制文件/文件夹是否追加
// 资源管理器(Explorer)的 Shell 扩展菜单项。
struct MenuConfig {
    bool open = true;          // 打开
    bool explore = true;       // 在资源管理器中打开
    bool terminal = true;      // 打开终端
    bool props = true;         // 属性
    bool copyPath = true;      // 复制完整路径
    bool copyName = true;      // 复制文件名
    bool copySize = true;      // 复制大小
    bool enter = true;         // 进入此目录
    bool locateList = true;    // 在列表中定位
    bool locateTreemap = true; // 在树图中定位
    bool selectParent = true;  // 选择父级
    bool goUp = true;          // 向上
    bool back = true;          // 后退
    bool forward = true;       // 前进
    bool rescan = true;        // 重新扫描此项
    bool deleteRecycle = true; // 删除到回收站
    bool deleteForever = true; // 永久删除
    bool shellMenu = true;     // 包含资源管理器扩展菜单
};

// ================= 应用设置（JSON 文件双向绑定 + 内置默认值） =================
struct AppSettings {
    // 扫描
    int threads = 8;         // 扫描线程数 1-16
    bool skipHidden = false; // 跳过隐藏/系统项
    bool followReparse = false; // 跟随重解析点（自动防环）

    // 树图
    int maxDepth = 10;       // 递归层级 1-20
    int minSizeMB = 1;       // 递归最小尺寸(MB) 0-8192
    int maxRects = 2000;     // 最大矩形数 100-50000
    int kidsCap = 120;       // 每文件夹展开上限 20-500
    int lazyDepth = 0;       // 懒加载层级 0=全量传输；层级之上全量、层级之下的子目录懒加载
    bool showLabels = true;  // 大矩形显示名称
    int colorScheme = 0;     // 0=按扩展名 1=按类型 2=单色
    // 面积算法（与 Web 版一致，共享 config.json）
    // 默认 3=√x + 0=先和→g(Σx)：与 web/index.html 的出厂默认（mapSel=sqrt / ordSel=sumG）一致
    int mapKind = 3;         // 映射 g：0=x 1=log₂ 2=log₂² 3=√x 4=x^α
    int ordKind = 0;         // 顺序：0=先和后 g(Σx) 1=先 g 后 Σg(x)
    double powAlpha = 0.5;   // 幂指数 α（mapKind==4 时生效 0.1-1.0）

    // 操作
    int defaultSort = 0;     // 0=大小降序 1=名称升序
    int theme = 0;           // 0=深色 1=浅色
    // 左侧列表行高（名称下方附加一行：大小 · 占比）；树图矩形高度不小于该值
    int rowExtraPx = 16;     // 0=关闭 8-48
    bool confirmDelete = true; // 删除到回收站前系统确认
    std::wstring lastPath;   // 上次扫描路径

    // 右键菜单（数据驱动）
    MenuConfig menu;
};

// 从 JSON 配置文件加载；文件不存在/损坏时写入内置默认值并返回默认。
// 每次保存都会写入完整 JSON（含全部默认键），保证"存默认"。
void LoadSettings(AppSettings& s);

// 将当前设置（含默认值）写回 JSON 配置文件。
void SaveSettings(const AppSettings& s);

// 获取配置文件路径（优先 exe 目录下的 config.json，不可写时退回 %APPDATA%\DiskMate）
std::wstring GetConfigPath();

// 设置对话框（扫描/树图/操作 三组 + 打开"右键菜单"子对话框）
bool ShowSettingsDialog(HWND parent, AppSettings& s);

// 右键菜单配置对话框（所有命令的开关，双向绑定到 AppSettings.menu）
bool ShowMenuConfigDialog(HWND parent, MenuConfig& m);

// 重命名对话框
bool ShowRenameDialog(HWND parent, std::wstring& name);
