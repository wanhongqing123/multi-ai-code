# Read the MaiChat version from the configured CMake project, not Electron's package.json.
param(
    [Parameter(Mandatory = $true)][string]$ProjectRoot,
    [Parameter(Mandatory = $true)][string]$BuildPath
)

$ErrorActionPreference = 'Stop'
$cachePath = Join-Path $BuildPath 'CMakeCache.txt'
$sourcePath = Join-Path $ProjectRoot 'CMakeLists.txt'
if (-not (Test-Path -LiteralPath $cachePath) -or -not (Test-Path -LiteralPath $sourcePath)) {
    throw 'MaiChat source and configured CMake cache are required before packaging'
}
$configured = Select-String -LiteralPath $cachePath -Pattern '^CMAKE_PROJECT_VERSION:STATIC=([0-9]+\.[0-9]+\.[0-9]+)$' |
    Select-Object -First 1
$source = Get-Content -LiteralPath $sourcePath -Raw -Encoding UTF8
if (-not $configured -or $source -notmatch '(?m)^project\(MaiChatDesktop VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES') {
    throw 'MaiChat project version is missing from source or CMake cache'
}
$cacheVersion = $configured.Matches[0].Groups[1].Value
$sourceVersion = $Matches[1]
if ($cacheVersion -ne $sourceVersion) {
    throw "MaiChat CMake cache version $cacheVersion differs from source version $sourceVersion"
}
Write-Output $cacheVersion
