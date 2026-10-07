; DiskMate 安装程序 — Inno Setup 6/7
;
;   输入: release\DiskMate_v1.0  （绿色版目录，三个前端都要先构建好）
;   输出: release\DiskMate_Setup.exe
;
;   重新打包的方法：
;     1) 安装 Inno Setup 6+（https://jrsoftware.org/isdl.php）
;     2) "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer\DiskMate.iss
;   注意：**先签名 exe，再编译安装器**，否则打进去的还是未签名文件。

#define SourceDir "D:\Desktop\diskmate\release\DiskMate_v1.0"
#define AppVer "1.0.0"

[Setup]
AppName=DiskMate
AppVersion={#AppVer}
AppVerName=DiskMate {#AppVer}
AppPublisher=DiskMate
AppComments=磁盘空间分析器 / Disk Space Analyzer
DefaultDirName={localappdata}\Programs\DiskMate
DefaultGroupName=DiskMate
DisableProgramGroupPage=yes
DisableDirPage=no
DisableReadyPage=no
UninstallDisplayIcon={app}\diskmate_imgui.exe
UninstallDisplayName=DiskMate 磁盘空间分析器
OutputDir=D:\Desktop\diskmate\release
OutputBaseFilename=DiskMate_Setup
Compression=lzma2/ultra64
SolidCompression=yes
InternalCompressLevel=ultra64
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
SetupIconFile={#SourceDir}\web\diskmate_tree.ico
WizardStyle=modern
ShowLanguageDialog=no
VersionInfoVersion={#AppVer}
VersionInfoDescription=DiskMate 安装程序
VersionInfoProductName=DiskMate
VersionInfoProductVersion={#AppVer}

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Default.isl,compiler:Languages\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加任务:"

[Files]
; ---- 三个前端（推荐 ImGui 版；装了 WebView2 也可以用 HTML 版）----
Source: "{#SourceDir}\diskmate_imgui.exe";  DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\diskmate_native.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\diskmate_web.exe";    DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\WebView2Loader.dll";  DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\web\*";               DestDir: "{app}\web"; Flags: ignoreversion recursesubdirs skipifsourcedoesntexist
; ---- 运行时数据与默认配置 ----
Source: "{#SourceDir}\diskmate.lib";        DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\diskmate.ini";        DestDir: "{app}"; Flags: onlyifdoesntexist uninsneveruninstall
Source: "{#SourceDir}\config.json";         DestDir: "{app}"; Flags: onlyifdoesntexist uninsneveruninstall
Source: "{#SourceDir}\ui-prefs.json";       DestDir: "{app}"; Flags: onlyifdoesntexist uninsneveruninstall
Source: "{#SourceDir}\theme.json";          DestDir: "{app}"; Flags: onlyifdoesntexist uninsneveruninstall
Source: "{#SourceDir}\使用说明.txt";          DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\DiskMate 磁盘空间分析器"; Filename: "{app}\diskmate_imgui.exe"; WorkingDir: "{app}"; IconFilename: "{app}\web\diskmate_tree.ico"
Name: "{autodesktop}\DiskMate 磁盘空间分析器"; Filename: "{app}\diskmate_imgui.exe"; WorkingDir: "{app}"; IconFilename: "{app}\web\diskmate_tree.ico"; Tasks: desktopicon

[Run]
Filename: "{app}\diskmate_imgui.exe"; Description: "运行 DiskMate"; Flags: nowait postinstall skipifsilent
Filename: "{app}\使用说明.txt"; Description: "查看使用说明"; Flags: shellexec postinstall skipifsilent unchecked

[UninstallDelete]
Type: filesandordirs; Name: "{app}\webview_data"
Type: files; Name: "{app}\diskmate.log"
Type: files; Name: "{app}\diskmate_imgui.log"
