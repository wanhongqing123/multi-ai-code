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

// Codex 二进制登记表：**全集**在这里列全，再用 bundled 决定这次打不打包。
//
// 以前这里是手写的两个名字，漏掉的东西无从发现——d31546e「FIX: 打包 Codex Code Mode host」
// 修的就是同一类问题（一个 helper 没被打包），但只补了当时注意到的那一个，没有回头把
// [[bin]] 目标数一遍，于是一个月后又暴露出别的遗漏。
//
// 所以现在是：登记全集 + 显式标注打不打包 + 构建后按表核对输出目录（缺失或零字节即失败）。
// 「不打包」是一个写下来的决定并附理由，而不是一个没人注意到的空缺。
const CODEX_BINARIES = [
  { name: 'codex', platforms: null, bundled: true },
  { name: 'codex-code-mode-host', platforms: null, bundled: true },
  // 以下两个只在 Windows 沙箱的 elevated 级别用得上，**当前不打包**（合计约 23.7MB）。
  //
  // 三个级别里只有 elevated 会碰它们：
  //   disabled    不隔离
  //   unelevated  以当前用户身份跑、受限令牌 —— 直接拉起子进程，不经过 command-runner
  //   elevated    以一个专门创建的 Windows 用户跑 —— setup 负责建这个用户，
  //               command-runner 以该用户身份接 IPC 并拉起子进程
  //
  // 而 elevated 只能靠 config.toml 里显式写 `[windows] sandbox = "elevated"` 选中：
  // 走 feature 那条路的 Feature::WindowsSandboxElevated 是
  // `stage: Removed, default_enabled: false`（features/src/lib.rs），已经不是活开关。
  //
  // 所以默认配置下它们一次都不会被调用，纯占体积。要开 elevated 时把 bundled 改成 true
  // 重建即可——留在表里而不是删掉，是为了保住上面这段结论，省得下次有人重新查一遍，
  // 或者反过来又漏掉它们（这个坑已经踩过两次）。
  { name: 'codex-windows-sandbox-setup', platforms: ['win32'], bundled: false },
  { name: 'codex-command-runner', platforms: ['win32'], bundled: false }
]

const requiredBinaries = CODEX_BINARIES.filter(
  (entry) =>
    entry.bundled && (entry.platforms === null || entry.platforms.includes(process.platform))
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

// 产物齐全性校验。copyExecutable 在源文件缺失时就会抛，所以这一步不是为了兜住"没复制"，
// 而是兜住"复制完之后又没了"——本文件上面那条注释已经写明这台机器上会发生什么：
// 企业终端安全软件对新复制的可执行文件做延迟扫描，可能把它隔离/删掉。
// 那种情况下每一步都返回成功，只有最终目录是缺的或零字节，而这正是会被打进安装包的东西。
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
