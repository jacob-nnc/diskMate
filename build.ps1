# DiskMate 一键构建（MinGW / MSVC）
#
#   .\build.ps1                     构建全部目标（Release）
#   .\build.ps1 -Target diskmate_imgui
#   .\build.ps1 -Clean              先删掉 build 目录
#   .\build.ps1 -Run                构建完直接启动 ImGui 版
#
# WebView2 缺失时只会跳过 diskmate_web，其它目标照常构建。
param(
    [string]$Target = "",
    [switch]$Clean,
    [switch]$Run,
    [string]$BuildDir = "build",
    [string]$Config = "Release"
)
$ErrorActionPreference = "Stop"
Push-Location $PSScriptRoot

function Info($m) { Write-Host "[build] $m" -ForegroundColor Cyan }
function Warn($m) { Write-Host "[build] $m" -ForegroundColor Yellow }
function Die($m)  { Write-Host "[build] $m" -ForegroundColor Red; Pop-Location; exit 1 }

# ---- 工具检查 ----
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { Die "找不到 cmake，请安装 CMake 3.20+ 并加入 PATH" }
$gen = "MinGW Makefiles"
if (-not (Get-Command g++ -ErrorAction SilentlyContinue)) {
    Warn "PATH 里没有 g++；如果用的是 MSVC，请改用 -G `"Visual Studio 17 2022`""
    $gen = "Visual Studio 17 2022"
}
Info ("cmake " + (cmake --version | Select-Object -First 1))

# ---- WebView2（可选）----
if (-not $env:DISKMATE_WEBVIEW2_DIR) {
    $nuget = Join-Path $env:USERPROFILE ".nuget\packages\microsoft.web.webview2\1.0.2210.55\build\native"
    if (Test-Path (Join-Path $nuget "include\WebView2.h")) { $env:DISKMATE_WEBVIEW2_DIR = $nuget }
    else { Warn "没找到 WebView2 SDK → 本次跳过 diskmate_web（装法见 README）" }
} else { Info "WebView2: $env:DISKMATE_WEBVIEW2_DIR" }

if ($Clean -and (Test-Path $BuildDir)) { Info "清理 $BuildDir"; Remove-Item $BuildDir -Recurse -Force }

# ---- 配置 + 构建 ----
Info "configure → $BuildDir ($gen)"
cmake -S . -B $BuildDir -G $gen -DCMAKE_BUILD_TYPE=$Config
if ($LASTEXITCODE -ne 0) { Die "cmake configure 失败" }

if ($Target) {
    Info "build target=$Target"
    cmake --build $BuildDir --target $Target -j 8
} else {
    Info "build all"
    cmake --build $BuildDir -j 8
}
if ($LASTEXITCODE -ne 0) { Die "编译失败" }

$exe = Join-Path $BuildDir "diskmate_imgui.exe"
Get-ChildItem $BuildDir -Filter "*.exe" | Where-Object { $_.Name -like "diskmate*" } |
    ForEach-Object { Info ("产物: " + $_.FullName + "  " + [math]::Round($_.Length/1MB,2) + " MB") }

if ($Run -and (Test-Path $exe)) { Info "启动 $exe"; Start-Process $exe }
Pop-Location
