# 打包 Windows 免安装绿色版：exe + Qt DLL + VC 运行库 + ImSDK，解压即用，
# 目标机器无需安装 Qt 或 VC++ 运行库（Win10+ 自带 UCRT）。
#
# 用法（先完成 Release 构建）：
#   powershell -ExecutionPolicy Bypass -File scripts\package-windows.ps1
#   可选参数：-BuildDir build-msvc2019_64   -OutDir dist
#
# 产出：
#   <OutDir>\MultiAIIM-win64\            解压即用目录
#   <OutDir>\MultiAIIM-win64-<日期>-<git短哈希>.zip

param(
    [string]$BuildDir = 'build-msvc2019_64',
    [string]$OutDir = 'dist',
    # 只组装 staging 目录不出 zip（供安装程序脚本 make-installer-windows.ps1 复用）。
    [switch]$SkipZip
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot

$buildPath = Join-Path $projectRoot $BuildDir
$exePath = Join-Path $buildPath 'multi_ai_im_desktop.exe'
if (-not (Test-Path $exePath)) {
    throw "未找到 $exePath，请先构建：cmake --build $BuildDir --target multi_ai_im_desktop"
}

# 从 CMakeCache 定位 Qt（避免依赖 PATH）
$cachePath = Join-Path $buildPath 'CMakeCache.txt'
$qt5DirLine = Select-String -Path $cachePath -Pattern '^Qt5_DIR:PATH=(.+)$'
if (-not $qt5DirLine) { throw "CMakeCache.txt 里没有 Qt5_DIR，无法定位 windeployqt" }
$qt5Dir = $qt5DirLine.Matches[0].Groups[1].Value
$qtBin = (Resolve-Path (Join-Path $qt5Dir '..\..\..\bin')).Path
$windeployqt = Join-Path $qtBin 'windeployqt.exe'
if (-not (Test-Path $windeployqt)) { throw "未找到 $windeployqt" }

# 组装 staging 目录
$distRoot = Join-Path $projectRoot $OutDir
$staging = Join-Path $distRoot 'MultiAIIM-win64'
if (Test-Path $staging) { Remove-Item $staging -Recurse -Force }
New-Item -ItemType Directory -Force $staging | Out-Null

Copy-Item $exePath (Join-Path $staging 'multi_ai_im_desktop.exe')

# windeployqt 旁挂 Qt 运行时；--no-translations 减小体积
# （应用界面文案为中文硬编码，不依赖 Qt 翻译文件）。
# 注意：不用 --compiler-runtime——它依赖 vcvars 环境变量定位 VC 运行库，
# 环境不满足时会静默跳过，下面改为显式拷贝，缺失即报错。
& $windeployqt --release --no-translations `
    --dir $staging (Join-Path $staging 'multi_ai_im_desktop.exe')
if ($LASTEXITCODE -ne 0) { throw "windeployqt 失败，退出码 $LASTEXITCODE" }

# 显式旁挂 VC++ 运行库（app-local 部署）：exe 与 Qt5*.dll 都依赖
# MSVCP140/VCRUNTIME140 系列，未装 VC Redist 的机器上缺它们会直接 0xc0000135。
# 从本机 VS 的 Redist 目录取最新版本的 x64 CRT 全套。
# 按 MSVC\<版本号> 解析并取最高版本：运行库版本必须 >= 编译工具集版本，
# 按路径字母序会被安装盘符/目录名干扰（如 VS2022 的 14.44 排在 BuildTools 14.50 后面）。
$crtDirs = Get-ChildItem 'C:\Program Files*\Microsoft Visual Studio\*\*\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT' `
    -ErrorAction SilentlyContinue |
    Sort-Object { [version]($_.FullName -replace '.*\\MSVC\\([\d.]+)\\.*', '$1') }
if (-not $crtDirs) { throw '未找到 VC Redist CRT 目录（Microsoft.VC*.CRT），无法旁挂 VC 运行库' }
$crtDir = $crtDirs[-1].FullName
Copy-Item (Join-Path $crtDir '*.dll') $staging -Force
foreach ($required in 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') {
    if (-not (Test-Path (Join-Path $staging $required))) {
        throw "VC 运行库旁挂后仍缺 $required（来源 $crtDir）"
    }
}
Write-Host "VC 运行库已旁挂（来源 $crtDir）"

# ImSDK 动态库：DynamicTimSdkApi 按 <exe 目录>/vendor/tencent-im/... 相对路径探测，
# 打包时必须保持该目录结构。
$imSdkSource = Join-Path $projectRoot 'vendor\tencent-im\windows\shared_lib\Win64\ImSDK.dll'
if (-not (Test-Path $imSdkSource)) { throw "未找到 $imSdkSource" }
$imSdkTargetDir = Join-Path $staging 'vendor\tencent-im\windows\shared_lib\Win64'
New-Item -ItemType Directory -Force $imSdkTargetDir | Out-Null
Copy-Item $imSdkSource $imSdkTargetDir

# OpenSSL 1.1：Qt 5.15 的 QNetworkAccessManager 走 HTTPS 依赖它，windeployqt 不会携带。
# 缺失时 QSslSocket::supportsSsl()==false，接收到的图片/文件（腾讯 IM 给的是 HTTPS URL，
# 需本端下载）会静默失败——表现为"文字能收、图片收不到"。显式旁挂，缺失即报错。
$opensslDir = Join-Path $projectRoot 'vendor\openssl\win64'
foreach ($ssl in 'libssl-1_1-x64.dll', 'libcrypto-1_1-x64.dll') {
    $sslSrc = Join-Path $opensslDir $ssl
    if (-not (Test-Path $sslSrc)) { throw "未找到 $sslSrc（接收图片/文件的 HTTPS 下载需 OpenSSL 1.1）" }
    Copy-Item $sslSrc $staging -Force
}
Write-Host 'OpenSSL 1.1 已旁挂（HTTPS 图片/文件下载所需）'

# 使用说明
$readme = @"
MaiChat 桌面客户端（Windows 免安装版）
==========================================

运行：双击 multi_ai_im_desktop.exe。

- 无需安装 Qt 或 VC++ 运行库，全部依赖已随包附带（需 Windows 10 及以上 64 位）。
- 首次启动在登录页输入账号 ID 后回车即可登录（UserSig 由内置密钥本地生成）。
- 聊天记录等本地数据存放于当前用户目录，删除本目录即可完成"卸载"。
- vendor\ 目录存放腾讯 IM SDK 动态库，请勿移动或删除。
"@
[System.IO.File]::WriteAllText(
    (Join-Path $staging '使用说明.txt'),
    $readme,
    [System.Text.UTF8Encoding]::new($true)
)

if ($SkipZip) {
    Write-Host ""
    Write-Host "staging 组装完成（跳过 zip）："
    Write-Host "  目录: $staging"
    return
}

# 压缩，文件名带日期与 git 短哈希便于追溯
$gitHash = (& git -C $projectRoot rev-parse --short HEAD 2>$null)
if (-not $gitHash) { $gitHash = 'unknown' }
$zipName = "MultiAIIM-win64-$(Get-Date -Format yyyyMMdd)-$gitHash.zip"
$zipPath = Join-Path $distRoot $zipName
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
Compress-Archive -Path (Join-Path $staging '*') -DestinationPath $zipPath

$sizeMB = [math]::Round((Get-Item $zipPath).Length / 1MB, 1)
Write-Host ""
Write-Host "打包完成："
Write-Host "  目录: $staging"
Write-Host "  压缩包: $zipPath （$sizeMB MB）"
