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
  stripReleaseExecutable,
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

// 只编 codex-cli：我们唯一要的产物就是它的 codex.exe。裸 `cargo build` 会编整个
// workspace，2026-08 rebase 到 openai/codex main 后这会直接构建失败——上游新增的
// code-mode-runtime 打开了 v8 的 v8_enable_sandbox feature，v8 的 build script 转而
// 去下 rusty_v8_ptrcomp_sandbox_release_x86_64-pc-windows-msvc.lib.gz，而 rusty_v8
// v150.4.0 的 release 里根本没发布 Windows 的 ptrcomp_sandbox 变体（只有 plain 和
// simdutf），404 后 panic。codex-cli 的依赖图里没有 v8（cargo tree -p codex-cli -i v8
// 无匹配），限定包即可绕开，顺带也省掉一堆用不上的 crate。
const cargoPackage = ['-p', 'codex-cli']
const cargoArgs =
  profile === 'dev' ? ['build', ...cargoPackage] : ['build', '--release', ...cargoPackage]
run('cargo', cargoArgs, { cwd: codexRsRoot })
copyExecutable(builtBinary, outputBinary)
if (profile === 'release' && stripReleaseExecutable(outputBinary)) {
  console.log(`Codex release binary stripped：${outputBinary}`)
}

writeManifestEntry({
  tool: 'codex',
  platformArch: platform,
  sourceCommit: gitCommit(codexRoot),
  version: tryVersion(outputBinary),
  binaryPath: outputBinary
})

console.log(`Codex 已构建：${outputBinary}`)
