param(
    [Parameter(Mandatory = $true)][string]$SourceDirectory,
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [Parameter(Mandatory = $true)][string]$MsysBash,
    [int]$Jobs = 6
)

$ErrorActionPreference = 'Stop'

function ConvertTo-MsysPath([string]$Path) {
    $absolute = [System.IO.Path]::GetFullPath($Path).Replace('\', '/')
    if ($absolute -notmatch '^([A-Za-z]):/(.*)$') {
        throw "FFmpeg build path must be on a Windows drive: $absolute"
    }
    return '/' + $Matches[1].ToLowerInvariant() + '/' + $Matches[2]
}

$source = (Resolve-Path -LiteralPath $SourceDirectory).Path
$build = [System.IO.Path]::GetFullPath($BuildDirectory)
$bash = (Resolve-Path -LiteralPath $MsysBash).Path
$msysBin = Split-Path -Parent $bash
if (-not (Test-Path -LiteralPath (Join-Path $msysBin 'make.exe'))) {
    throw 'MSYS2 make.exe is required. Install it with: pacman -S make diffutils pkgconf'
}
foreach ($program in @('clang-cl.exe', 'lld-link.exe', 'lib.exe')) {
    if (-not (Get-Command $program -ErrorAction SilentlyContinue)) {
        throw "$program is required in PATH; run from an MSVC x64 developer environment with LLVM installed"
    }
}
New-Item -ItemType Directory -Path $build -Force | Out-Null

# MSYS2 login shells otherwise drop the Visual Studio and LLVM compiler paths.
$env:MSYS2_PATH_TYPE = 'inherit'
$env:MAICHAT_FFMPEG_SOURCE = ConvertTo-MsysPath $source
$env:MAICHAT_FFMPEG_BUILD = ConvertTo-MsysPath $build
$env:MAICHAT_FFMPEG_JOBS = [string][Math]::Max(1, $Jobs)
$script = @'
set -euo pipefail
mkdir -p "$MAICHAT_FFMPEG_BUILD"
cd "$MAICHAT_FFMPEG_BUILD"
"$MAICHAT_FFMPEG_SOURCE/configure" \
  --prefix="$MAICHAT_FFMPEG_BUILD/install" \
  --toolchain=msvc --cc=clang-cl.exe --ld=lld-link.exe --ar=lib.exe \
  --target-os=win64 --arch=x86_64 \
  --disable-programs --disable-doc --disable-debug --disable-autodetect --disable-asm \
  --disable-gpl --disable-nonfree --enable-static --disable-shared --enable-pic \
  --extra-cflags=-MD
make -j"$MAICHAT_FFMPEG_JOBS"
make -j"$MAICHAT_FFMPEG_JOBS" libmaifftools.lib
make install
'@
$ErrorActionPreference = 'Continue'
& $bash -lc $script
$buildExit = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($buildExit -ne 0) { throw "FFmpeg build failed with exit code $buildExit" }

foreach ($library in @('avdevice', 'avfilter', 'avformat', 'avcodec', 'swresample', 'swscale', 'avutil')) {
    $archive = Join-Path $build "lib$library/$library.lib"
    if (-not (Test-Path -LiteralPath $archive)) { throw "Missing FFmpeg archive: $archive" }
}
if (-not (Test-Path -LiteralPath (Join-Path $build 'libmaifftools.lib'))) {
    throw 'Missing embedded FFmpeg command archive'
}
if (-not (Test-Path -LiteralPath (Join-Path $build 'install/include/libavfilter/avfilter.h'))) {
    throw 'FFmpeg public headers were not installed'
}
