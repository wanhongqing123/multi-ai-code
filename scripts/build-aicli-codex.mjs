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
const builtBinary = join(codexRsRoot, 'target', 'debug', binaryName('codex'))

requireDir(codexRsRoot, 'Codex submodule')
requireCommand('cargo')

run('cargo', ['build'], { cwd: codexRsRoot })
copyExecutable(builtBinary, outputBinary)

writeManifestEntry({
  tool: 'codex',
  platformArch: platform,
  sourceCommit: gitCommit(codexRoot),
  version: tryVersion(outputBinary),
  binaryPath: outputBinary
})

console.log(`Codex 已构建：${outputBinary}`)
