# DiskMate 代码签名
#
#   .\sign.ps1 -Pfx C:\cert\codesign.pfx                     # 用 .pfx 证书
#   .\sign.ps1 -Pfx C:\cert\codesign.pfx -Password (Read-Host -AsSecureString)
#   .\sign.ps1 -Thumbprint 0123ABCD...                       # 用证书库里已安装的证书
#   .\sign.ps1 -Verify                                       # 只检查当前签名状态
#   .\sign.ps1 -Path ..\release\DiskMate_v1.0                # 指定要签的目录（默认就是它）
#   .\sign.ps1 -SelfTestCert                                 # 无证书时生成自签名证书跑通流程（仅本机测试）
#
# 为什么必须带时间戳（/tr）：证书一过期，没打时间戳的签名会全部失效。
param(
    [string]$Path = "",
    [string]$Pfx = "",
    [System.Security.SecureString]$Password,
    [string]$Thumbprint = "",
    [string]$Timestamp = "http://timestamp.digicert.com",
    [switch]$Verify,
    [switch]$SelfTestCert,
    [switch]$Force
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
if (-not $Path) {
    # 依次找常见的发布目录（源码包与发布包通常是两个文件夹）
    $cands = @(
        (Join-Path $root "release\DiskMate_v1.0"),
        (Join-Path $root "dist\DiskMate-v1.0-win64"),
        (Join-Path (Split-Path $root -Parent) "release\DiskMate_v1.0")
    )
    foreach ($c in $cands) { if (Test-Path $c) { $Path = $c; break } }
}
if (-not $Path -or -not (Test-Path $Path)) {
    Write-Host "找不到发布目录，请显式指定：  .\sign.ps1 -Path D:\path\to\DiskMate_v1.0" -ForegroundColor Red
    exit 1
}
Write-Host "发布目录: $Path" -ForegroundColor Cyan

$files = Get-ChildItem $Path -Recurse -Include *.exe,*.dll -File
if (-not $files) { Write-Host "目录里没有 exe/dll: $Path" -ForegroundColor Red; exit 1 }

function Show-Status {
    Write-Host "== 签名状态 ==" -ForegroundColor Cyan
    foreach ($f in $files) {
        $s = Get-AuthenticodeSignature $f.FullName
        $color = switch ($s.Status) { "Valid" { "Green" } "NotSigned" { "Yellow" } default { "Red" } }
        Write-Host ("{0,-26} {1,-12} {2}" -f $f.Name, $s.Status, $s.SignerCertificate.Subject) -ForegroundColor $color
    }
}

if ($Verify) { Show-Status; exit 0 }

# ---------------- 取证书 ----------------
$cert = $null
$useStore = $true
if ($Pfx) {
    if (-not (Test-Path $Pfx)) { Write-Host "找不到证书文件: $Pfx" -ForegroundColor Red; exit 1 }
    $cert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($Pfx, $Password)
    $useStore = $false
    Write-Host "使用证书: $($cert.Subject)（有效期至 $($cert.NotAfter)）" -ForegroundColor Cyan
} elseif ($Thumbprint) {
    $cert = Get-ChildItem Cert:\CurrentUser\My, Cert:\LocalMachine\My |
            Where-Object { $_.Thumbprint -eq $Thumbprint } | Select-Object -First 1
    if (-not $cert) { Write-Host "证书库找不到指纹 $Thumbprint" -ForegroundColor Red; exit 1 }
    Write-Host "使用证书: $($cert.Subject)" -ForegroundColor Cyan
} elseif ($SelfTestCert) {
    Write-Host "生成自签名证书（只用来验证签名流程；不会消除 SmartScreen 的发布者未知提示）..." -ForegroundColor Yellow
    # 首选系统 API；写证书库被拒时（权限/沙箱）退化成「纯文件 pfx」，完全不动证书库
    $useStore = $true
    try {
        $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=DiskMate Self-Test" `
                -KeyUsage DigitalSignature -CertStoreLocation Cert:\CurrentUser\My `
                -NotAfter (Get-Date).AddYears(1)
        Write-Host "  指纹: $($cert.Thumbprint)" -ForegroundColor Yellow
    } catch {
        Write-Host "  写入证书库被拒，改为生成文件证书（不碰证书库）" -ForegroundColor DarkYellow
        $rsa = [System.Security.Cryptography.RSA]::Create(2048)
        $req = New-Object System.Security.Cryptography.X509Certificates.CertificateRequest(
                   "CN=DiskMate Self-Test", $rsa,
                   [System.Security.Cryptography.HashAlgorithmName]::SHA256,
                   [System.Security.Cryptography.RSASignaturePadding]::Pkcs1)
        $req.CertificateExtensions.Add((New-Object System.Security.Cryptography.X509Certificates.X509BasicConstraintsExtension($false, $false, 0, $false)))
        $req.CertificateExtensions.Add((New-Object System.Security.Cryptography.X509Certificates.X509KeyUsageExtension(
            [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::DigitalSignature, $false)))
        $oidsc = New-Object System.Security.Cryptography.OidCollection
        [void]$oidsc.Add((New-Object System.Security.Cryptography.Oid("1.3.6.1.5.5.7.3.3")))   # 代码签名 EKU
        $req.CertificateExtensions.Add((New-Object System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension($oidsc, $false)))
        $cert = $req.CreateSelfSigned([DateTimeOffset]::Now.AddDays(-1), [DateTimeOffset]::Now.AddYears(1))
        $selftestPfx = Join-Path $PSScriptRoot "diskmate-selftest.pfx"
        $selftestPwd = "diskmate"
        [IO.File]::WriteAllBytes($selftestPfx, $cert.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Pfx, $selftestPwd))
        $Pfx = $selftestPfx
        $Password = ConvertTo-SecureString $selftestPwd -AsPlainText -Force
        $useStore = $false
        Write-Host "  已生成: $selftestPfx  (密码 $selftestPwd)" -ForegroundColor Yellow
        Write-Host "  不想留着就删：Remove-Item '$selftestPfx'" -ForegroundColor DarkGray
    }
} else {
    Write-Host "没有可用的代码签名证书。`n" -ForegroundColor Yellow
    Write-Host "代码签名必须有一张证书："
    Write-Host "  · CA 签发的 OV/EV 代码签名证书（DigiCert / Sectigo / GlobalSign 等，年费几百到几千元）"
    Write-Host "  · 或 Azure Trusted Signing（按次计费，个人开发者也能申请）"
    Write-Host "  自签名证书只能本机测试，不会消除 SmartScreen 的发布者未知提示。`n"
    Write-Host "拿到证书后重新运行："
    Write-Host "    .\sign.ps1 -Pfx D:\cert\codesign.pfx"
    Write-Host "    .\sign.ps1 -Thumbprint <证书指纹>"
    exit 2
}

# ---------------- signtool（有就用，没有就 PowerShell 兜底）----------------
$signtool = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin\*\x64\signtool.exe" -ErrorAction SilentlyContinue |
            Sort-Object FullName | Select-Object -Last 1
Write-Host ("signtool: " + $(if ($signtool) { $signtool.FullName } else { "未找到，改用 Set-AuthenticodeSignature" })) -ForegroundColor Cyan

# .pfx 走临时导出（signtool 只吃明文密码，别把它留在命令行历史里）
$tmpPfx = $null; $tmpPwd = $null
if (-not $useStore) {
    $tmpPwd = [guid]::NewGuid().ToString("N")
    $tmpPfx = Join-Path $env:TEMP ("dm_sign_" + $tmpPwd + ".pfx")
    [IO.File]::WriteAllBytes($tmpPfx, $cert.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Pfx, $tmpPwd))
}

foreach ($f in $files) {
    # 已经签好而且有效的（例如微软签名的 WebView2Loader.dll）不要重签
    $cur = Get-AuthenticodeSignature $f.FullName
    if ($cur.Status -eq "Valid" -and -not $Force) {
        Write-Host ("跳过 " + $f.Name + "（已有有效签名）") -ForegroundColor DarkGray
        continue
    }
    Write-Host ("签名 " + $f.Name + " ...") -ForegroundColor Gray
    if ($signtool) {
        if ($useStore) {
            & $signtool.FullName sign /fd SHA256 /tr $Timestamp /td SHA256 /sha1 $cert.Thumbprint $f.FullName
        } else {
            & $signtool.FullName sign /fd SHA256 /tr $Timestamp /td SHA256 /f $tmpPfx /p $tmpPwd $f.FullName
        }
        if ($LASTEXITCODE -ne 0) {
            Write-Host "  signtool 失败（退出码 $LASTEXITCODE），改用 PowerShell 兜底" -ForegroundColor Yellow
            $s = Set-AuthenticodeSignature -FilePath $f.FullName -Certificate $cert -TimestampServer $Timestamp
            Write-Host ("  → " + $s.Status)
        }
    } else {
        $s = Set-AuthenticodeSignature -FilePath $f.FullName -Certificate $cert -TimestampServer $Timestamp
        Write-Host ("  → " + $s.Status)
    }
}
if ($tmpPfx) { Remove-Item $tmpPfx -Force -ErrorAction SilentlyContinue }

Write-Host ""
Show-Status
if ($SelfTestCert -and $cert) {
    Write-Host "`n清理这张自签名证书：" -ForegroundColor Yellow
    Write-Host "  Get-ChildItem Cert:\CurrentUser\My | Where-Object { `$_.Subject -like '*DiskMate Self-Test*' } | Remove-Item"
}
