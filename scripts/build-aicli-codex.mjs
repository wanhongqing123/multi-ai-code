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
const outputBinary = join(outputDir, binaryName('codex'))
const outputCodeModeHost = join(outputDir, binaryName('codex-code-mode-host'))

// 默认 release：dev profile 的 codex.exe 带调试信息约 358MB 且未优化，
// 打进安装包体积和速度都不可接受。日常改代码迭代可用 CODEX_BUILD_PROFILE=dev
// 走增量 debug 构建（秒级），发布打包前再跑一次默认 release。
// （opencode 无此区分：bun 单文件编译始终 minify + 无 sourcemap。）
const profile = process.env.CODEX_BUILD_PROFILE === 'dev' ? 'dev' : 'release'
const builtBinary = join(
  codexRsRoot,
  'target',
  profile === 'dev' ? 'debug' : 'release',
  binaryName('codex')
)
const builtCodeModeHost = join(
  codexRsRoot,
  'target',
  profile === 'dev' ? 'debug' : 'release',
  binaryName('codex-code-mode-host')
)

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

// 只构建安装包实际需要的两个二进制，避免重新构建整个 Codex workspace。
const cargoBinaries = ['--bin', 'codex', '--bin', 'codex-code-mode-host']
const cargoArgs =
  profile === 'dev' ? ['build', ...cargoBinaries] : ['build', '--release', ...cargoBinaries]
run('cargo', cargoArgs, { cwd: codexRsRoot, env: v8Env })
// 先在构建目录验证原始产物并读取版本，再复制/strip。企业终端安全软件可能对
// 新复制的可执行文件做延迟扫描，不能让这种本机策略把 manifest 静默写成 null。
const builtVersion = requireVersion(builtBinary)
copyExecutable(builtBinary, outputBinary)
copyExecutable(builtCodeModeHost, outputCodeModeHost)
if (profile === 'release') {
  for (const binary of [outputBinary, outputCodeModeHost]) {
    if (stripReleaseExecutable(binary)) {
      console.log(`Codex release binary stripped：${binary}`)
    }
  }
}

writeManifestEntry({
  tool: 'codex',
  platformArch: platform,
  sourceCommit: gitCommit(codexRoot),
  version: builtVersion,
  binaryPath: outputBinary,
  helperPaths: {
    codeModeHost: outputCodeModeHost
  }
})

console.log(`Codex 已构建：${outputBinary}`)
console.log(`Codex Code Mode host 已构建：${outputCodeModeHost}`)
