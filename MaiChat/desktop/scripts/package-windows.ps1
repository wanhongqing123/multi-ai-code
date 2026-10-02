# 打包 Windows 免安装绿色版：exe + Qt DLL + VC 运行库 + ImSDK，解压即用，
# 目标机器无需安装 Qt 或 VC++ 运行库（Win10+ 自带 UCRT）。
#
# 用法（先完成 Release 构建）：
#   powershell -ExecutionPolicy Bypass -File scripts\package-windows.ps1
#   可选参数：-BuildDir build-msvc2019_64   -OutDir dist
#
# 产出：
#   <OutDir>\MaiChat-win64\            解压即用目录
#   <OutDir>\MaiChat-win64-v<版本>-<日期>-<git短哈希>.zip

param(
    [string]$BuildDir = 'build-msvc2019_64',
    [string]$OutDir = 'dist',
    # 只组装 staging 目录不出 zip（供安装程序脚本 make-installer-windows.ps1 复用）。
    [switch]$SkipZip
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot

$buildPath = Join-Path $projectRoot $BuildDir
# OpenCV 的 CMake 子项目会设置 EXECUTABLE_OUTPUT_PATH。必须按当前构建缓存
# 定位新生成的程序，不能误用构建目录根部残留的旧 maichat.exe。
$cachePath = Join-Path $buildPath 'CMakeCache.txt'
if (-not (Test-Path $cachePath)) { throw "未找到 $cachePath，请先配置 Release 构建" }
$ffmpegEnabled = Select-String -Path $cachePath -Pattern '^MAICHAT_ENABLE_FFMPEG:BOOL=(ON|TRUE|1)$' -Quiet
$opencvEnabled = Select-String -Path $cachePath -Pattern '^MAICHAT_ENABLE_OPENCV:BOOL=(ON|TRUE|1)$' -Quiet
$runtimeOutputLine = Select-String -Path $cachePath -Pattern '^EXECUTABLE_OUTPUT_PATH:PATH=(.+)$'
$exeDir = $buildPath
if ($runtimeOutputLine) {
    $runtimeOutput = $runtimeOutputLine.Matches[0].Groups[1].Value
    if ([System.IO.Path]::IsPathRooted($runtimeOutput)) {
        $exeDir = $runtimeOutput
    } else {
        $exeDir = Join-Path $buildPath $runtimeOutput
    }
}
$exePath = Join-Path $exeDir 'maichat.exe'
if (-not (Test-Path $exePath)) {
    throw "未找到 $exePath，请先构建：cmake --build $BuildDir --target maichat"
}

# 从 CMakeCache 定位 Qt（避免依赖 PATH）
$qt5DirLine = Select-String -Path $cachePath -Pattern '^Qt5_DIR:PATH=(.+)$'
if (-not $qt5DirLine) { throw "CMakeCache.txt 里没有 Qt5_DIR，无法定位 windeployqt" }
$qt5Dir = $qt5DirLine.Matches[0].Groups[1].Value
$qtBin = (Resolve-Path (Join-Path $qt5Dir '..\..\..\bin')).Path
$windeployqt = Join-Path $qtBin 'windeployqt.exe'
if (-not (Test-Path $windeployqt)) { throw "未找到 $windeployqt" }

# 组装 staging 目录
$distRoot = Join-Path $projectRoot $OutDir
$staging = Join-Path $distRoot 'MaiChat-win64'
if (Test-Path $staging) { Remove-Item $staging -Recurse -Force }
New-Item -ItemType Directory -Force $staging | Out-Null

Copy-Item $exePath (Join-Path $staging 'maichat.exe')

# windeployqt 旁挂 Qt 运行时；--no-translations 减小体积
# （应用界面文案为中文硬编码，不依赖 Qt 翻译文件）。
# 注意：不用 --compiler-runtime——它依赖 vcvars 环境变量定位 VC 运行库，
# 环境不满足时会静默跳过，下面改为显式拷贝，缺失即报错。
$previousErrorAction = $ErrorActionPreference
try {
    # Without VCINSTALLDIR, windeployqt writes a warning to stderr even though
    # it succeeds; the CRT is explicitly copied below.
    $ErrorActionPreference = 'Continue'
    & $windeployqt --release --no-translations `
        --dir $staging (Join-Path $staging 'maichat.exe')
    $deployExitCode = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $previousErrorAction
}
if ($deployExitCode -ne 0) { throw "windeployqt 失败，退出码 $deployExitCode" }

# 视频播放的解码后端是插件，不是 Qt5Multimedia.dll 本身。缺了它 QMediaPlayer
# 不报错也不崩，只是永远停在 StoppedState、画面全黑——和当年缺 OpenSSL 导致
# 「文字能收、图片收不到」是同一类静默故障。windeployqt 在部分环境不会带上，
# 因此从同一套 Qt 的插件目录显式复制并校验。
$mediaserviceDir = Join-Path $staging 'mediaservice'
$qtMediaServiceDir = Join-Path (Split-Path -Parent $qtBin) 'plugins\mediaservice'
New-Item -ItemType Directory -Force $mediaserviceDir | Out-Null
foreach ($plugin in 'wmfengine.dll', 'dsengine.dll', 'qtmedia_audioengine.dll') {
    $source = Join-Path $qtMediaServiceDir $plugin
    if (Test-Path -LiteralPath $source) {
        Copy-Item -LiteralPath $source -Destination (Join-Path $mediaserviceDir $plugin) -Force
    }
}
$mediaBackends = @(Get-ChildItem $mediaserviceDir -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -in 'wmfengine.dll', 'dsengine.dll' })
if ($mediaBackends.Count -eq 0) {
    throw "staging 里没有 mediaservice 解码插件（wmfengine/dsengine），视频消息将无法播放"
}
Write-Host "多媒体解码插件已部署：$($mediaBackends.Name -join ', ')"

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

# TRTC 运行时（远程桌面）：liteav.dll 等必须与 exe 同目录，否则应用启动时
# 直接弹「找不到 liteav.dll」而根本进不去——不是远程桌面不可用而已，是整个
# 程序起不来（隐式链接，加载期就要求）。
# 以构建产物为准判断是否需要：CMake 只在 TRTC 可用时才把这些 DLL 拷到 exe
# 旁边，所以构建目录里有就说明 exe 真的依赖它们；关掉远程桌面编译时则跳过。
$trtcBuildDlls = @(Get-ChildItem (Join-Path $exeDir '*.dll') -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -in 'liteav.dll', 'liteav_screen.dll', 'txffmpeg.dll', 'txsoundtouch.dll' })
if ($trtcBuildDlls.Count -gt 0) {
    $trtcDir = Join-Path $projectRoot 'vendor\tencent-trtc\windows\lib\x64'
    foreach ($dll in 'liteav.dll', 'liteav_screen.dll', 'txffmpeg.dll', 'txsoundtouch.dll') {
        $trtcSrc = Join-Path $trtcDir $dll
        if (-not (Test-Path $trtcSrc)) { throw "未找到 $trtcSrc（远程桌面所需的 TRTC 运行时）" }
        Copy-Item $trtcSrc $staging -Force
    }
    Write-Host 'TRTC 运行时已旁挂（远程桌面所需）'
} else {
    Write-Host 'TRTC 运行时未参与构建，跳过旁挂（本包不含远程桌面）'
}

if ($ffmpegEnabled) {
    foreach ($dll in 'maichat_ffplay_hosted.dll', 'maiagent_graphics.dll',
                     'maiagent_obs_d3d11.dll', 'maiagent_obs_pthreads.dll') {
        $source = Join-Path $exeDir $dll
        if (-not (Test-Path -LiteralPath $source)) {
            throw "进程内 FFplay/Graphics 构建产物缺失：$source"
        }
        Copy-Item -LiteralPath $source -Destination (Join-Path $staging $dll) -Force
    }
    $effectsSource = Join-Path $exeDir 'MaiAgentGraphics'
    foreach ($effect in 'default.effect', 'video_conversion.effect') {
        if (-not (Test-Path -LiteralPath (Join-Path $effectsSource $effect))) {
            throw "Graphics effect 缺失：$effect"
        }
    }
    $effectsTarget = Join-Path $staging 'MaiAgentGraphics'
    New-Item -ItemType Directory -Force $effectsTarget | Out-Null
    Copy-Item -Path (Join-Path $effectsSource '*') -Destination $effectsTarget -Recurse -Force
    Write-Host '进程内 FFplay/Graphics 运行库及 effect 已打包'
}

# RVM 的 ONNX Runtime 通过 LoadLibraryW 动态加载，windeployqt 不会发现它。
# 与 CMake 的资产开关一致：仅在 FFmpeg/OpenCV 和官方 Windows 运行时齐备时打包。
$repositoryRoot = (Resolve-Path (Join-Path $projectRoot '..\..')).Path
$rvmSourceModel = Join-Path $repositoryRoot 'MaiAgent\models\rvm_mobilenetv3_fp32.onnx'
$ortSourceRuntime = Join-Path $repositoryRoot 'MaiAgent\third_party\onnxruntime\windows-x64\onnxruntime.dll'
$ortSourceProvider = Join-Path $repositoryRoot 'MaiAgent\third_party\onnxruntime\windows-x64\onnxruntime_providers_shared.dll'
if ($ffmpegEnabled -and $opencvEnabled -and
    (Test-Path -LiteralPath $rvmSourceModel) -and
    (Test-Path -LiteralPath $ortSourceRuntime) -and
    (Test-Path -LiteralPath $ortSourceProvider)) {
    foreach ($asset in @(
        'onnxruntime.dll',
        'onnxruntime_providers_shared.dll',
        'MaiAgentModels\rvm_mobilenetv3_fp32.onnx',
        'MaiAgentModels\RVM_LICENSE',
        'MaiAgentModels\ONNXRUNTIME_LICENSE',
        'MaiAgentModels\ONNXRUNTIME_ThirdPartyNotices.txt'
    )) {
        $source = Join-Path $exeDir $asset
        if (-not (Test-Path -LiteralPath $source)) {
            throw "Windows RVM 构建产物缺失：$source"
        }
        $target = Join-Path $staging $asset
        New-Item -ItemType Directory -Force (Split-Path -Parent $target) | Out-Null
        Copy-Item -LiteralPath $source -Destination $target -Force
    }
    Write-Host 'RVM 模型、ONNX Runtime DLL 和许可文件已打包'
} else {
    Write-Host 'Windows RVM 资产未启用，跳过打包（cv_video_matting 不注册）'
}

# 使用说明
$readme = @"
MaiChat 桌面客户端（Windows 免安装版）
==========================================

运行：双击 maichat.exe。

- 无需安装 Qt 或 VC++ 运行库，全部依赖已随包附带（需 Windows 10 及以上 64 位）。
- 首次启动在登录页输入账号 ID 后回车即可登录（UserSig 由内置密钥本地生成）。
- 聊天记录等本地数据存放于当前用户目录，删除本目录即可完成"卸载"。
- vendor\ 目录存放腾讯 IM SDK 动态库，请勿移动或删除。
- MaiAgentModels\ 与 onnxruntime*.dll 是视频抠像资源，请勿移动或删除。
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
# 版本号取仓库根 package.json，和 make-installer-windows.ps1 同一个来源。
# 名字里带上版本，是因为 Release 页面上光看 zip 名认不出它属于哪一版
# ——安装包和 macOS 的 dmg 都带，唯独 zip 不带的话，三个产物摆在一起会以为漏了。
$repoRoot = Split-Path -Parent (Split-Path -Parent $projectRoot)
$pkgJson = Join-Path $repoRoot 'package.json'
$appSemver = (Get-Content $pkgJson -Raw -Encoding UTF8 | ConvertFrom-Json).version
if (-not $appSemver) { throw "无法从 $pkgJson 读取 version" }
$zipName = "MaiChat-win64-v$appSemver-$(Get-Date -Format yyyyMMdd)-$gitHash.zip"
$zipPath = Join-Path $distRoot $zipName
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
# ZipFile reads the staged files directly; PowerShell 5.1 Compress-Archive can
# report a false sharing violation for the exe it has just copied.
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory(
    $staging, $zipPath, [System.IO.Compression.CompressionLevel]::Optimal, $false)

$sizeMB = [math]::Round((Get-Item $zipPath).Length / 1MB, 1)
Write-Host ""
Write-Host "打包完成："
Write-Host "  目录: $staging"
Write-Host "  压缩包: $zipPath （$sizeMB MB）"
