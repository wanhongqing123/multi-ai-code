# Validate the executable's PE imports before starting a Windows GUI process.
# A missing import fails before main() and can leave a system error dialog open.
param(
    [Parameter(Mandatory = $true)][string]$AppDir,
    [Parameter(Mandatory = $true)][string]$DumpbinPath
)

$ErrorActionPreference = 'Stop'
$exe = Join-Path $AppDir 'maichat.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "Missing executable: $exe" }
if (-not (Test-Path -LiteralPath $DumpbinPath)) { throw "Missing dumpbin: $DumpbinPath" }

$output = & $DumpbinPath /dependents $exe
if ($LASTEXITCODE -ne 0) { throw "dumpbin /dependents failed: $LASTEXITCODE" }
$inImports = $false
$imports = New-Object System.Collections.Generic.List[string]
foreach ($line in $output) {
    if ($line -match 'Image has the following dependencies:') {
        $inImports = $true
        continue
    }
    if ($inImports -and $line -match '^\s*Image has the following') { break }
    if ($inImports -and $line -match '^\s*([A-Za-z0-9._-]+\.dll)\s*$') {
        $imports.Add($Matches[1])
    }
}
if ($imports.Count -eq 0) { throw "No PE imports found in $exe" }

$missing = New-Object System.Collections.Generic.List[string]
foreach ($name in ($imports | Select-Object -Unique)) {
    # API-set DLL names are virtual contracts resolved by the Windows loader.
    if ($name -match '^(api|ext)-ms-win-') { continue }
    if ((Test-Path -LiteralPath (Join-Path $AppDir $name)) -or
        (Test-Path -LiteralPath (Join-Path $env:WINDIR "System32\$name"))) {
        continue
    }
    $missing.Add($name)
}
foreach ($plugin in 'platforms\qwindows.dll', 'sqldrivers\qsqlite.dll') {
    if (-not (Test-Path -LiteralPath (Join-Path $AppDir $plugin))) {
        $missing.Add($plugin)
    }
}
if ($missing.Count -gt 0) {
    throw "Missing MaiChat runtime dependencies in ${AppDir}: $($missing -join ', ')"
}
Write-Host "MaiChat runtime verified: $($imports.Count) PE imports and Qt plugins"
