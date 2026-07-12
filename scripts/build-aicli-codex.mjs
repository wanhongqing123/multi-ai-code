import { join } from 'path'
import {
  binaryName,
  copyExecutable,
  gitCommit,
  platformArch,
  repoRoot,
  requireCommand,
  requireDir,
  run,
  tryVersion,
  writeManifestEntry
} from './aicli-build-utils.mjs'

const codexRoot = join(repoRoot, 'third_party', 'aicli', 'codex')
const codexRsRoot = join(codexRoot, 'codex-rs')
const platform = platformArch()
const outputBinary = join(repoRoot, 'bin', 'aicli', 'codex', platform, binaryName('codex'))

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

requireDir(codexRsRoot, 'Codex submodule')
requireCommand('cargo')

const cargoArgs = profile === 'dev' ? ['build'] : ['build', '--release']
run('cargo', cargoArgs, { cwd: codexRsRoot })
copyExecutable(builtBinary, outputBinary)

writeManifestEntry({
  tool: 'codex',
  platformArch: platform,
  sourceCommit: gitCommit(codexRoot),
  version: tryVersion(outputBinary),
  binaryPath: outputBinary
})

console.log(`Codex 已构建：${outputBinary}`)
