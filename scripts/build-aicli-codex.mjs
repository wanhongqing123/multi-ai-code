import { existsSync, statSync } from 'fs'
import { join } from 'path'
import {
  binaryName,
  capture,
  copyExecutable,
  gitCommit,
  platformArch,
  repoRoot,
  requireCommand,
  requireDir,
  resolvePythonCommand,
  run,
  rustTargetForPlatform,
  stripReleaseExecutable,
  requireVersion,
  writeManifestEntry
} from './aicli-build-utils.mjs'

const codexRoot = join(repoRoot, 'third_party', 'aicli', 'codex')
const codexRsRoot = join(codexRoot, 'codex-rs')
const platform = platformArch()
const outputDir = join(repoRoot, 'bin', 'aicli', 'codex', platform)

// 打包所需的全部 Codex 二进制。以前这里是手写的两个名字，结果漏掉了 Windows 沙箱的
// 两个 helper：codex 运行时到 codex.exe 旁边找它们（helper_materialization.rs 的
// bundled_executable_path_for_exe），找不到就起不了沙箱，本该在沙箱里跑的只读命令
// 只能退回提权/审批那条路——症状不是报错，而是「每条命令都要提权」，看着像设计如此。
//
// 更要命的是这个坑踩过两次：d31546e「FIX: 打包 Codex Code Mode host」修的就是同一类
// 问题，但只补了当时发现的那一个，没有回头把 [[bin]] 目标数一遍。所以现在改成显式登记
// 全集 + 构建后校验缺失即失败，让下一次遗漏在构建时炸掉，而不是几个月后由用户在使用中发现。
const CODEX_BINARIES = [
  { name: 'codex', platforms: null },
  { name: 'codex-code-mode-host', platforms: null },
  // Windows 沙箱专有：非 Windows 目标上这两个 crate 的 build.rs 直接 early return，
  // setup_main 在非 Windows 甚至是 panic! 桩，所以只在 win32 上要求它们。
  { name: 'codex-windows-sandbox-setup', platforms: ['win32'] },
  { name: 'codex-command-runner', platforms: ['win32'] }
]

const requiredBinaries = CODEX_BINARIES.filter(
  (entry) => entry.platforms === null || entry.platforms.includes(process.platform)
).map((entry) => ({
  name: entry.name,
  fileName: binaryName(entry.name),
  outputPath: join(outputDir, binaryName(entry.name))
}))

const outputBinary = requiredBinaries.find((entry) => entry.name === 'codex').outputPath

// 默认 release：dev profile 的 codex.exe 带调试信息约 358MB 且未优化，
// 打进安装包体积和速度都不可接受。日常改代码迭代可用 CODEX_BUILD_PROFILE=dev
// 走增量 debug 构建（秒级），发布打包前再跑一次默认 release。
// （opencode 无此区分：bun 单文件编译始终 minify + 无 sourcemap。）
const profile = process.env.CODEX_BUILD_PROFILE === 'dev' ? 'dev' : 'release'
const targetDir = join(codexRsRoot, 'target', profile === 'dev' ? 'debug' : 'release')
for (const entry of requiredBinaries) {
  entry.builtPath = join(targetDir, entry.fileName)
}
const builtBinary = requiredBinaries.find((entry) => entry.name === 'codex').builtPath

// codex-code-mode-host -> codeModeHost，保持 manifest 里既有的 camelCase 键名不变。
function helperManifestKey(binName) {
  return binName
    .replace(/^codex-/, '')
    .split('-')
    .map((part, index) => (index === 0 ? part : part[0].toUpperCase() + part.slice(1)))
    .join('')
}

requireDir(codexRsRoot, 'Codex submodule')
requireCommand('cargo')

// Code Mode 通过同目录下的独立 host 执行工具。Codex 锁定的 rusty_v8 crate、静态库
// 和 binding 必须严格匹配；复用上游校验逻辑，从 OpenAI release 自动准备对应产物。
const python = resolvePythonCommand()
const v8EnvJson = capture(
  python.command,
  [
    ...python.prefixArgs,
    join(repoRoot, 'scripts', 'resolve-codex-v8-env.py'),
    rustTargetForPlatform()
  ],
  { cwd: repoRoot }
)
const v8Env = JSON.parse(v8EnvJson)

// 只构建安装包实际需要的这几个二进制，避免重新构建整个 Codex workspace。
const cargoBinaries = requiredBinaries.flatMap((entry) => ['--bin', entry.name])
const cargoArgs =
  profile === 'dev' ? ['build', ...cargoBinaries] : ['build', '--release', ...cargoBinaries]
run('cargo', cargoArgs, { cwd: codexRsRoot, env: v8Env })
// 先在构建目录验证原始产物并读取版本，再复制/strip。企业终端安全软件可能对
// 新复制的可执行文件做延迟扫描，不能让这种本机策略把 manifest 静默写成 null。
const builtVersion = requireVersion(builtBinary)
for (const entry of requiredBinaries) {
  copyExecutable(entry.builtPath, entry.outputPath)
}
if (profile === 'release') {
  for (const entry of requiredBinaries) {
    if (stripReleaseExecutable(entry.outputPath)) {
      console.log(`Codex release binary stripped：${entry.outputPath}`)
    }
  }
}

// 产物齐全性校验。copyExecutable 缺源文件时会抛，但这里再按「登记的全集」核一遍输出目录：
// 真正要防的是「登记表加了一项、却没有任何一步真的把它放进去」这类改动，
// 那种情况下每一步都不报错，只有最终目录是缺的。
const missing = requiredBinaries.filter(
  (entry) => !existsSync(entry.outputPath) || statSync(entry.outputPath).size === 0
)
if (missing.length > 0) {
  throw new Error(
    `Codex 打包产物缺失或为空：${missing.map((entry) => entry.fileName).join(', ')}`
  )
}

writeManifestEntry({
  tool: 'codex',
  platformArch: platform,
  sourceCommit: gitCommit(codexRoot),
  version: builtVersion,
  binaryPath: outputBinary,
  helperPaths: Object.fromEntries(
    requiredBinaries
      .filter((entry) => entry.name !== 'codex')
      .map((entry) => [helperManifestKey(entry.name), entry.outputPath])
  )
})

for (const entry of requiredBinaries) {
  console.log(`Codex 已构建：${entry.outputPath}`)
}
