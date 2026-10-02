param(
    [Parameter(Mandatory = $true)][string]$SourceDirectory,
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [Parameter(Mandatory = $true)][string]$MsysBash,
    [Parameter(Mandatory = $true)][string]$Meson,
    [int]$Jobs = 6
)

$ErrorActionPreference = 'Stop'

function ConvertTo-MsysPath([string]$Path) {
    $absolute = [System.IO.Path]::GetFullPath($Path).Replace('\', '/')
    if ($absolute -notmatch '^([A-Za-z]):/(.*)$') {
        throw "Media codec build path must be on a Windows drive: $absolute"
    }
    return '/' + $Matches[1].ToLowerInvariant() + '/' + $Matches[2]
}

function Assert-ExitCode([string]$Operation) {
    if ($LASTEXITCODE -ne 0) { throw "$Operation failed with exit code $LASTEXITCODE" }
}

$source = (Resolve-Path -LiteralPath $SourceDirectory).Path
$build = [System.IO.Path]::GetFullPath($BuildDirectory)
$bash = (Resolve-Path -LiteralPath $MsysBash).Path
$mesonProgram = (Resolve-Path -LiteralPath $Meson).Path
$mesonPackageRoot = Split-Path -Parent (Split-Path -Parent $mesonProgram)
if (Test-Path -LiteralPath (Join-Path $mesonPackageRoot 'mesonbuild')) {
    $env:PYTHONPATH = $mesonPackageRoot
}
$install = Join-Path $build 'install'
$lib = Join-Path $install 'lib'
$include = Join-Path $install 'include'
foreach ($program in @('cl.exe', 'lib.exe', 'msbuild.exe', 'nasm.exe')) {
    if (-not (Get-Command $program -ErrorAction SilentlyContinue)) {
        throw "$program is required in an MSVC x64 developer environment"
    }
}
New-Item -ItemType Directory -Force $build, $install, $lib, $include | Out-Null

$dav1dSource = Join-Path $source 'dav1d'
$dav1dBuild = Join-Path $build 'dav1d'
if (Test-Path -LiteralPath (Join-Path $dav1dBuild 'build.ninja')) {
    & $mesonProgram setup --reconfigure $dav1dBuild $dav1dSource -Denable_asm=true
} else {
    & $mesonProgram setup $dav1dBuild $dav1dSource "--prefix=$install" --libdir=lib `
        --default-library=static -Denable_asm=true -Denable_tools=false `
        -Denable_tests=false -Denable_examples=false -Denable_docs=false
}
Assert-ExitCode 'dav1d configuration'
& $mesonProgram compile -C $dav1dBuild "-j$Jobs"
Assert-ExitCode 'dav1d build'
& $mesonProgram install -C $dav1dBuild
Assert-ExitCode 'dav1d install'
Copy-Item (Join-Path $lib 'libdav1d.a') (Join-Path $lib 'dav1d.lib') -Force

$env:MSYS2_PATH_TYPE = 'inherit'
$env:MAICHAT_CODEC_SOURCE = ConvertTo-MsysPath $source
$env:MAICHAT_CODEC_BUILD = ConvertTo-MsysPath $build
$env:MAICHAT_CODEC_JOBS = [string][Math]::Max(1, $Jobs)
$x264Script = @'
set -euo pipefail
mkdir -p "$MAICHAT_CODEC_BUILD/x264"
cd "$MAICHAT_CODEC_BUILD/x264"
CC=cl.exe AS=nasm.exe AR=lib.exe RANLIB=: "$MAICHAT_CODEC_SOURCE/x264/configure" \
  --host=x86_64-w64-mingw32 --prefix="$MAICHAT_CODEC_BUILD/install" \
  --libdir="$MAICHAT_CODEC_BUILD/install/lib" --enable-static --disable-cli \
  --disable-opencl --bit-depth=8
make clean
make -j"$MAICHAT_CODEC_JOBS"
make install-lib-static
'@
$x264ScriptFile = Join-Path $build 'build-maichat-x264.sh'
[System.IO.File]::WriteAllText($x264ScriptFile, $x264Script,
    [System.Text.UTF8Encoding]::new($false))
& $bash -l (ConvertTo-MsysPath $x264ScriptFile)
Assert-ExitCode 'x264 build'
$x264Config = Join-Path $build 'x264/config.mak'
if (-not (Select-String -LiteralPath $x264Config -Pattern '^AS=nasm(\.exe)?$' -Quiet)) {
    throw 'x264 built without NASM assembly; check its configure log'
}
$x264Pc = Join-Path $lib 'pkgconfig/x264.pc'
$msysInstall = ConvertTo-MsysPath $install
$windowsInstall = $install.Replace('\', '/')
$pcContents = [System.IO.File]::ReadAllText($x264Pc)
[System.IO.File]::WriteAllText($x264Pc, $pcContents.Replace($msysInstall, $windowsInstall))

$lameProject = Join-Path $source 'lame/vc_solution/vs2019_libmp3lame.vcxproj'
$lameOutput = Join-Path $build 'lame/out/'
$lameIntermediate = Join-Path $build 'lame/obj/'
New-Item -ItemType Directory -Force $lameOutput, $lameIntermediate | Out-Null
$lameConfig = Join-Path $source 'lame/config.h'
$hadConfig = Test-Path -LiteralPath $lameConfig
$previousClFlags = $env:_CL_
try {
    $env:_CL_ = '/MD'
    & msbuild.exe $lameProject '/p:Configuration=Release' '/p:Platform=x64' `
        "/p:OutDir=$lameOutput" "/p:IntDir=$lameIntermediate" `
        '/p:PreLinkEventUseInBuild=false' '/m' '/v:minimal'
    Assert-ExitCode 'LAME build'
} finally {
    $env:_CL_ = $previousClFlags
    if (-not $hadConfig -and (Test-Path -LiteralPath $lameConfig)) {
        Remove-Item -LiteralPath $lameConfig
    }
}
Copy-Item (Join-Path $lameOutput 'libmp3lame-static.lib') `
    (Join-Path $lib 'libmp3lame.lib') -Force
Copy-Item (Join-Path $lib 'libmp3lame.lib') (Join-Path $lib 'mp3lame.lib') -Force
New-Item -ItemType Directory -Force (Join-Path $include 'lame') | Out-Null
Copy-Item (Join-Path $source 'lame/include/lame.h') `
    (Join-Path $include 'lame/lame.h') -Force

$zlibSource = Join-Path $source 'opencv/3rdparty/zlib'
$zlibBuild = Join-Path $build 'zlib'
New-Item -ItemType Directory -Force $zlibBuild | Out-Null
Copy-Item (Join-Path $zlibSource 'zlib.h') (Join-Path $include 'zlib.h') -Force
Copy-Item (Join-Path $zlibSource 'zconf.h.in') (Join-Path $include 'zconf.h') -Force
$zlibObjects = @()
foreach ($name in @('adler32', 'compress', 'crc32', 'deflate', 'gzclose', 'gzlib',
                   'gzread', 'gzwrite', 'inflate', 'infback', 'inftrees', 'inffast',
                   'trees', 'uncompr', 'zutil')) {
    $object = Join-Path $zlibBuild "$name.obj"
    & cl.exe /nologo /MD /O2 /D_CRT_SECURE_NO_DEPRECATE `
        "/I$zlibSource" "/I$include" /c (Join-Path $zlibSource "$name.c") `
        "/Fo$object"
    Assert-ExitCode "zlib $name build"
    $zlibObjects += $object
}
& lib.exe /nologo "/OUT:$(Join-Path $lib 'zlib.lib')" @zlibObjects
Assert-ExitCode 'zlib archive'

foreach ($archive in @('dav1d.lib', 'libx264.lib', 'mp3lame.lib', 'zlib.lib')) {
    if (-not (Test-Path -LiteralPath (Join-Path $lib $archive))) {
        throw "Missing media codec archive: $archive"
    }
}
