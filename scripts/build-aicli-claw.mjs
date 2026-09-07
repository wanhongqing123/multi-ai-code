import { existsSync, statSync } from 'fs'
import { spawnSync } from 'child_process'
import { join } from 'path'
import {
  binaryName,
  copyExecutable,
  gitCommit,
  platformArch,
  repoRoot,
  requireCommand,
  assertExecutableRuns,
  requireDir,
  run,
  stripReleaseExecutable,
  writeManifestEntry
} from './aicli-build-utils.mjs'

const clawRoot = join(repoRoot, 'third_party', 'aicli', 'claw')
const clawRsRoot = join(clawRoot, 'rust')
const platform = platformArch()
const outputDir = join(repoRoot, 'bin', 'aicli', 'claw', platform)

// Claw 二进制登记表：**全集**在这里列全，再用 bundled 决定这次打不打包。
// 和 Codex 那张表同样的理由——「不打包」要是一个写下来的决定并附理由，
// 而不是一个没人注意到的空缺（那个坑在 Codex 侧已经踩过两次）。
//
// 全集来自 rust/crates/*/Cargo.toml 的 [[bin]] 目标 + 带 src/main.rs 的 crate。
const CLAW_BINARIES = [
  // 主 CLI，来自 crate rusty-claude-cli。
  { name: 'claw', platforms: null, bundled: true },
  // NDJSON 契约二进制（schema / format_version，带 doctor 子命令）。
  // 第二步接结构化输出时很可能要用它，但**现在还没有任何代码调用**，
  // 打进去纯占体积，所以先不打包；等真正接上再把 bundled 改成 true。
  { name: 'claw-analog', platforms: null, bundled: false },
  // 检索服务和 Anthropic mock，都是上游自测设施，产品路径一次都不会调用。
  { name: 'claw-rag-service', platforms: null, bundled: false },
  { name: 'mock-anthropic-service', platforms: null, bundled: false }
]

const requiredBinaries = CLAW_BINARIES.filter(
  (entry) =>
    entry.bundled && (entry.platforms === null || entry.platforms.includes(process.platform))
).map((entry) => ({
  name: entry.name,
  fileName: binaryName(entry.name),
  outputPath: join(outputDir, binaryName(entry.name))
}))

const outputBinary = requiredBinaries.find((entry) => entry.name === 'claw').outputPath

// 与 Codex 同策略：默认 release，日常迭代可用 CLAW_BUILD_PROFILE=dev 走增量 debug 构建。
const profile = process.env.CLAW_BUILD_PROFILE === 'dev' ? 'dev' : 'release'
const targetDir = join(clawRsRoot, 'target', profile === 'dev' ? 'debug' : 'release')
for (const entry of requiredBinaries) {
  entry.builtPath = join(targetDir, entry.fileName)
}
const builtBinary = requiredBinaries.find((entry) => entry.name === 'claw').builtPath

// `claw --version` 是多行的：
//     Claw Code
//       Version          0.1.3
//       Git SHA          08106b0c3771
// 共享的 requireVersion 只取第一行，对 claw 会把 "Claw Code" 当成版本号写进 manifest。
// 所以这里自己解析 Version 那行；解析不出来必须抛，不能让 manifest 静默写成一个假值。
function requireClawVersion(binaryPath) {
  const result = spawnSync(binaryPath, ['--version'], {
    encoding: 'utf8',
    stdio: ['ignore', 'pipe', 'pipe'],
    shell: false
  })
  if (result.error || result.status !== 0) {
    throw new Error(`Claw 构建产物无法执行 --version：${binaryPath}`)
  }
  const output = `${result.stdout ?? ''}${result.stderr ?? ''}`
  const matched = output.match(/^\s*Version\s+(\S+)\s*$/m)
  if (!matched) {
    throw new Error(
      `Claw --version 输出里找不到版本号，格式可能变了：${binaryPath}\n${output.trim()}`
    )
  }
  return matched[1]
}

requireDir(clawRsRoot, 'Claw submodule')
requireCommand('cargo')

// 只构建实际要打包的二进制，避免重建整个 workspace（含 mock 服务和 RAG 服务）。
const cargoBinaries = requiredBinaries.flatMap((entry) => ['--bin', entry.name])
const cargoArgs =
  profile === 'dev' ? ['build', ...cargoBinaries] : ['build', '--release', ...cargoBinaries]
run('cargo', cargoArgs, { cwd: clawRsRoot })

// 先在构建目录验证原始产物并读取版本，再复制/strip。企业终端安全软件可能对新复制的
// 可执行文件做延迟扫描，不能让这种本机策略把 manifest 静默写成 null。
const builtVersion = requireClawVersion(builtBinary)
for (const entry of requiredBinaries) {
  copyExecutable(entry.builtPath, entry.outputPath)
}
if (profile === 'release') {
  for (const entry of requiredBinaries) {
    if (stripReleaseExecutable(entry.outputPath)) {
      console.log(`Claw release binary stripped：${entry.outputPath}`)
    }
  }
}

// 产物齐全性校验：兜的是「复制完之后又没了」——见上面那条关于延迟扫描的注释。
const missing = requiredBinaries.filter(
  (entry) => !existsSync(entry.outputPath) || statSync(entry.outputPath).size === 0
)
if (missing.length > 0) {
  throw new Error(`Claw 打包产物缺失或为空：${missing.map((entry) => entry.fileName).join(', ')}`)
}

// 存在且非零字节**不够**：macOS 的 strip 会把 Rust 产物改坏——文件在、体积正常、
// 签名也过，但一跑就被 SIGKILL、没有任何输出。原先只在 copy/strip 之前验过原始二进制，
// 这里补上「对最终落盘的那一个再执行一次」。
for (const entry of requiredBinaries) {
  assertExecutableRuns(entry.outputPath)
}

writeManifestEntry({
  tool: 'claw',
  platformArch: platform,
  sourceCommit: gitCommit(clawRoot),
  version: builtVersion,
  binaryPath: outputBinary,
  helperPaths: Object.fromEntries(
    requiredBinaries.filter((entry) => entry.name !== 'claw').map((entry) => [entry.name, entry.outputPath])
  )
})

for (const entry of requiredBinaries) {
  console.log(`Claw 已构建：${entry.outputPath}`)
}
