param(
    [Parameter(Mandatory = $true)][string]$SourceDir,
    [Parameter(Mandatory = $true)][string]$BuildDir
)

$ErrorActionPreference = 'Stop'
foreach ($tool in @('perl', 'nmake')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "$tool is required to build vendored OpenSSL; run from a Visual Studio developer prompt with Perl installed"
    }
}
New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
Push-Location $BuildDir
try {
    if (-not (Test-Path 'Makefile')) {
        & perl (Join-Path $SourceDir 'Configure') 'VC-WIN64A' 'no-shared' 'no-tests' `
            'no-apps' 'no-ssl' 'no-module' 'no-async' 'no-legacy' 'no-comp' `
            "--prefix=$(Join-Path $BuildDir 'install')"
        if ($LASTEXITCODE -ne 0) { throw 'OpenSSL Configure failed' }
    }
    & nmake build_generated
    if ($LASTEXITCODE -ne 0) { throw 'OpenSSL generated-header build failed' }
    & nmake libcrypto.lib
    if ($LASTEXITCODE -ne 0) { throw 'OpenSSL libcrypto build failed' }
} finally {
    Pop-Location
}
