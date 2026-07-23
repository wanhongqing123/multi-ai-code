# 构建 Windows 安装程序（NSIS）：staging（复用 package-windows.ps1）→ makensis。
#
# 用法（先完成 Release 构建）：
#   powershell -ExecutionPolicy Bypass -File scripts\make-installer-windows.ps1
#   可选参数：-BuildDir build-msvc2019_64   -OutDir dist   -MakeNsis <makensis.exe 路径>
#
# 产出：
#   <OutDir>\MaiChat-Setup-win64-<日期>-<git短哈希>.exe
#
# makensis 来源：优先用 -MakeNsis 指定；否则找 PATH，再退回 electron-builder
# 的 NSIS 缓存（Electron 打包时下载的同一套 NSIS，本机无需另装）。

param(
    [string]$BuildDir = 'build-msvc2019_64',
    [string]$OutDir = 'dist',
    [string]$MakeNsis = ''
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot

# 1) 组装 staging（不出 zip）
& (Join-Path $PSScriptRoot 'package-windows.ps1') -BuildDir $BuildDir -OutDir $OutDir -SkipZip
$staging = Join-Path (Join-Path $projectRoot $OutDir) 'MaiChat-win64'
if (-not (Test-Path (Join-Path $staging 'maichat.exe'))) {
    throw "staging 目录异常：$staging"
}

# 2) 覆盖使用说明为安装版文案（staging 里默认是绿色版文案）
$readme = @"
MaiChat 桌面客户端（Windows）
=================================

- 从开始菜单或桌面快捷方式启动。
- 首次启动在登录页输入账号 ID 后回车即可登录（UserSig 由内置密钥本地生成）。
- 聊天记录等本地数据存放于 %APPDATA%\MaiChat\Desktop IM，卸载时不会删除。
- 卸载：设置 → 应用 → 已安装的应用 → MaiChat。
- vendor\ 目录存放腾讯 IM SDK 动态库，请勿移动或删除。
"@
[System.IO.File]::WriteAllText(
    (Join-Path $staging '使用说明.txt'),
    $readme,
    [System.Text.UTF8Encoding]::new($true)
)

# 3) 定位 makensis
if (-not $MakeNsis) {
    $cmd = Get-Command makensis -ErrorAction SilentlyContinue
    if ($cmd) { $MakeNsis = $cmd.Source }
}
if (-not $MakeNsis) {
    $cached = Get-ChildItem "$env:LOCALAPPDATA\electron-builder\Cache\nsis" -Recurse -Filter makensis.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\Bin\\makensis\.exe$' } |
        Sort-Object FullName | Select-Object -Last 1
    if ($cached) { $MakeNsis = $cached.FullName }
}
if (-not $MakeNsis -or -not (Test-Path $MakeNsis)) {
    throw '未找到 makensis.exe：请安装 NSIS 或先运行一次 Electron 的 npm run dist:win（electron-builder 会缓存 NSIS）'
}
Write-Host "makensis: $MakeNsis"

# 4) 编译安装程序
# 版本号取仓库根 package.json 的 version——Electron 与 qt-im 共用同一版本源，
# 每次发布前通过 `npm version patch` 递增，两端安装包版本一致。
$repoRoot = (Resolve-Path (Join-Path $projectRoot '..\..')).Path
$pkgJsonPath = Join-Path $repoRoot 'package.json'
$appSemver = (Get-Content $pkgJsonPath -Raw -Encoding UTF8 | ConvertFrom-Json).version
if (-not $appSemver) { throw "无法从 $pkgJsonPath 读取 version" }
$gitHash = (& git -C $projectRoot rev-parse --short HEAD 2>$null)
if (-not $gitHash) { $gitHash = 'unknown' }
$appVersion = "$appSemver-$gitHash"   # 显示版本，如 0.1.1-abc1234
$versionNum = "$appSemver.0"          # NSIS VIProductVersion 需要 x.x.x.x
$outFile = Join-Path (Join-Path $projectRoot $OutDir) "MaiChat-Setup-win64-v$appSemver-$gitHash.exe"
if (Test-Path $outFile) { Remove-Item $outFile -Force }

$nsiScript = Join-Path $projectRoot 'installer\windows-installer.nsi'
$iconFile = Join-Path $projectRoot 'resources\windows\AppIcon.ico'

# /INPUTCHARSET UTF8：.nsi 为无 BOM UTF-8，含中文文案，不指定会按系统码页误读。
& $MakeNsis /INPUTCHARSET UTF8 `
    "/DSTAGING_DIR=$staging" `
    "/DOUT_FILE=$outFile" `
    "/DAPP_VERSION=$appVersion" `
    "/DAPP_VERSION_NUM=$versionNum" `
    "/DICON_FILE=$iconFile" `
    $nsiScript
if ($LASTEXITCODE -ne 0) { throw "makensis 失败，退出码 $LASTEXITCODE" }
if (-not (Test-Path $outFile)) { throw "makensis 成功但未生成 $outFile" }

$sizeMB = [math]::Round((Get-Item $outFile).Length / 1MB, 1)
Write-Host ""
Write-Host "安装程序构建完成："
Write-Host "  $outFile （$sizeMB MB）"
