param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDir
)

$ErrorActionPreference = 'Stop'
$sourceDir = Join-Path $PSScriptRoot '..\third_party\onnxruntime'
$buildScript = Join-Path $sourceDir 'build.bat'
if (-not (Test-Path $buildScript)) {
    throw "ONNX Runtime source is missing: $buildScript"
}

# Windows 使用 DirectML 调用 GPU，并启用 XNNPACK 的 CPU 优化。
# CUDA 和 TensorRT 依赖额外的 NVIDIA SDK，不能在普通 Windows 构建中无条件开启。
& $buildScript --config Release --build_dir $BuildDir --update --build --parallel `
    --skip_tests --skip_submodule_sync --build_shared_lib --use_dml --use_xnnpack `
    --cmake_extra_defines CMAKE_POLICY_VERSION_MINIMUM=3.5 `
    CMAKE_DISABLE_FIND_PACKAGE_Protobuf=ON onnxruntime_BUILD_UNIT_TESTS=OFF
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

# 桌面 App 只读取源码构建的产物目录，不再从 third_party 读取发布版 DLL。
$stageDir = Join-Path $PSScriptRoot '..\build\vendor\onnxruntime-windows-x64'
$releaseDir = Join-Path $BuildDir 'Release\Release'
if (-not (Test-Path (Join-Path $releaseDir 'onnxruntime.dll'))) {
    $releaseDir = Join-Path $BuildDir 'Release'
}
$runtime = Join-Path $releaseDir 'onnxruntime.dll'
if (-not (Test-Path $runtime)) {
    throw "ONNX Runtime build did not produce $runtime"
}
New-Item -ItemType Directory -Force -Path $stageDir | Out-Null
Copy-Item $runtime (Join-Path $stageDir 'onnxruntime.dll') -Force
$provider = Join-Path $releaseDir 'onnxruntime_providers_shared.dll'
if (Test-Path $provider) {
    Copy-Item $provider (Join-Path $stageDir 'onnxruntime_providers_shared.dll') -Force
}
