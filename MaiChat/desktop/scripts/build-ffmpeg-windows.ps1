param(
    [Parameter(Mandatory = $true)][string]$SourceDirectory,
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [Parameter(Mandatory = $true)][string]$MsysBash,
    [Parameter(Mandatory = $true)][string]$CodecInstallDirectory,
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
$codecs = (Resolve-Path -LiteralPath $CodecInstallDirectory).Path
$msysBin = Split-Path -Parent $bash
if (-not (Test-Path -LiteralPath (Join-Path $msysBin 'make.exe'))) {
    throw 'MSYS2 make.exe is required. Install it with: pacman -S make diffutils pkgconf'
}
foreach ($program in @('clang-cl.exe', 'lld-link.exe', 'lib.exe', 'nasm.exe')) {
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
$env:MAICHAT_CODEC_INCLUDE = ($codecs + '/include').Replace('\', '/')
$env:MAICHAT_CODEC_LIB = ($codecs + '/lib').Replace('\', '/')
$env:LIB = (Join-Path $codecs 'lib') + ';' + $env:LIB
$env:MAICHAT_PKG_CONFIG_PATH = ConvertTo-MsysPath (Join-Path $codecs 'lib/pkgconfig')
$script = @'
set -euo pipefail
export PKG_CONFIG_PATH="$MAICHAT_PKG_CONFIG_PATH"
export MSYS2_ARG_CONV_EXCL="/MD"
mkdir -p "$MAICHAT_FFMPEG_BUILD"
cd "$MAICHAT_FFMPEG_BUILD"
"$MAICHAT_FFMPEG_SOURCE/configure" \
  --prefix="$MAICHAT_FFMPEG_BUILD/install" \
  --toolchain=msvc --cc=clang-cl.exe --ld=lld-link.exe --ar=lib.exe \
  --target-os=win64 --arch=x86_64 \
  --disable-programs --disable-doc --disable-debug --disable-autodetect \
  --enable-gpl --disable-nonfree --enable-static --disable-shared --enable-pic \
  --enable-libdav1d --enable-libx264 --enable-libmp3lame --enable-zlib \
  --x86asmexe=nasm.exe \
  --extra-cflags="/MD -I$MAICHAT_CODEC_INCLUDE" \
  --extra-ldflags="-libpath:$MAICHAT_CODEC_LIB"
make clean
make -j"$MAICHAT_FFMPEG_JOBS"
make -j"$MAICHAT_FFMPEG_JOBS" libmaifftools.lib
make install
'@
$scriptFile = Join-Path $build 'build-maichat-ffmpeg.sh'
[System.IO.File]::WriteAllText($scriptFile, $script,
    [System.Text.UTF8Encoding]::new($false))
$ErrorActionPreference = 'Continue'
& $bash -l (ConvertTo-MsysPath $scriptFile)
$buildExit = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($buildExit -ne 0) { throw "FFmpeg build failed with exit code $buildExit" }
if (-not (Select-String -LiteralPath (Join-Path $build 'config.h') `
                       -Pattern '^#define HAVE_X86ASM 1$' -Quiet)) {
    throw 'FFmpeg built without x86 assembly; check the NASM toolchain and configure log'
}

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
