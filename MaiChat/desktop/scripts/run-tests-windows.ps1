# 文件作用：在 Windows 构建环境运行桌面端自动化检查。
# 阅读边界：此文件负责构建或测试流程，不在应用运行时解释为业务逻辑。
param(
    [string]$BuildDirectory = "build-agent",
    [string]$TestRegex = "",
    [switch]$VerboseOutput
)

$ErrorActionPreference = "Stop"

$desktopRoot = Split-Path -Parent $PSScriptRoot
$buildPath = [System.IO.Path]::GetFullPath((Join-Path $desktopRoot $BuildDirectory))
$cachePath = Join-Path $buildPath "CMakeCache.txt"
if (-not (Test-Path -LiteralPath $cachePath)) {
    throw "CMake cache not found: $cachePath"
}

$qtCoreEntry = Get-Content -LiteralPath $cachePath |
    Where-Object { $_ -match '^Qt5Core_DIR:PATH=' } |
    Select-Object -First 1
if (-not $qtCoreEntry) {
    throw "Qt5Core_DIR is missing from $cachePath"
}

$qtCoreDirectory = $qtCoreEntry.Substring($qtCoreEntry.IndexOf('=') + 1)
$qtBin = [System.IO.Path]::GetFullPath((Join-Path $qtCoreDirectory "..\..\..\bin"))
$qtTestDll = Join-Path $qtBin "Qt5Test.dll"
if (-not (Test-Path -LiteralPath $qtTestDll)) {
    throw "Qt test runtime not found: $qtTestDll"
}

# Every desktop test must enter through this script. Launching a test executable directly on
# Windows can omit Qt's bin directory and display a blocking system error dialog for Qt5Test.dll.
$qtSslDir = Join-Path $desktopRoot 'vendor\openssl\win64'
foreach ($name in 'libssl-1_1-x64.dll', 'libcrypto-1_1-x64.dll') {
    if (-not (Test-Path -LiteralPath (Join-Path $qtSslDir $name))) {
        throw "Qt SSL test runtime not found: $name"
    }
}
$env:PATH = "$qtBin;$qtSslDir;$env:PATH"

# Missing runtime DLLs and native crashes must be reported to the console, never as a modal system
# dialog that blocks an unattended test run.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class MaiChatTestErrorMode {
    [DllImport("kernel32.dll")]
    public static extern uint SetErrorMode(uint mode);
}
'@
[void][MaiChatTestErrorMode]::SetErrorMode(0x8003)

$arguments = @("--test-dir", $buildPath, "-C", "Release", "--output-on-failure")
if ($VerboseOutput) {
    $arguments += "-V"
}
if (-not [string]::IsNullOrWhiteSpace($TestRegex)) {
    $arguments += @("-R", $TestRegex)
}

& ctest @arguments
exit $LASTEXITCODE
