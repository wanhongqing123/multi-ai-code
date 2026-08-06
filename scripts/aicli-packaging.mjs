import { existsSync, readdirSync, statSync } from 'fs'
import { join } from 'path'
import { binaryName } from './aicli-build-utils.mjs'

function packagedResourcesDir(appOutDir, platform) {
  if (platform !== 'darwin') return join(appOutDir, 'resources')
  const appBundle = readdirSync(appOutDir).find((name) => name.endsWith('.app'))
  if (!appBundle) throw new Error(`macOS app bundle 不存在：${appOutDir}`)
  return join(appOutDir, appBundle, 'Contents', 'Resources')
}

export function verifyPackagedCodexBinaries(appOutDir, { platform, arch }) {
  const codexDir = join(
    packagedResourcesDir(appOutDir, platform),
    'app.asar.unpacked',
    'bin',
    'aicli',
    'codex',
    `${platform}-${arch}`
  )
  const required = [binaryName('codex', platform), binaryName('codex-code-mode-host', platform)]
  const missing = required.filter((name) => {
    const path = join(codexDir, name)
    return !existsSync(path) || !statSync(path).isFile()
  })
  if (missing.length > 0) {
    throw new Error(`Codex 打包产物缺失：${missing.join(', ')}（目录：${codexDir}）`)
  }
  return required.map((name) => join(codexDir, name))
}
