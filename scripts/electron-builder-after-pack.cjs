const { execFileSync } = require('node:child_process')
const { readdirSync, existsSync } = require('node:fs')
const { join } = require('node:path')

function archNameFromElectronBuilder(arch) {
  if (arch === 3 || arch === 'arm64') return 'arm64'
  if (arch === 0 || arch === 'ia32') return 'ia32'
  return 'x64'
}

function findMacAppBundle(appOutDir) {
  if (!existsSync(appOutDir)) return null
  const appName = readdirSync(appOutDir).find((name) => name.endsWith('.app'))
  return appName ? join(appOutDir, appName) : null
}

function adHocSignMacApp(appOutDir) {
  const appBundle = findMacAppBundle(appOutDir)
  if (!appBundle) {
    console.log('[afterPack] macOS app bundle not found, skip ad-hoc codesign')
    return
  }
  try {
    execFileSync('xattr', ['-cr', appBundle], { stdio: 'inherit' })
  } catch {
    // xattr is best-effort; codesign below is the important verification step.
  }
  execFileSync('codesign', ['--force', '--deep', '--sign', '-', appBundle], {
    stdio: 'inherit'
  })
  execFileSync('codesign', ['--verify', '--deep', '--strict', '--verbose=2', appBundle], {
    stdio: 'inherit'
  })
  console.log(`[afterPack] macOS app ad-hoc signed: ${appBundle}`)
}

module.exports = async function afterPack(context) {
  const { prunePackagedAsrResources } = await import('./asr-packaging.mjs')
  const platform = context.electronPlatformName
  const arch = archNameFromElectronBuilder(context.arch)
  const result = prunePackagedAsrResources(context.appOutDir, { platform, arch })
  if (result.missing) {
    console.log(`[afterPack] ASR resources not found for ${platform}-${arch}`)
  } else {
    console.log(
      `[afterPack] ASR resources kept for ${platform}-${arch}: ${result.kept.join(', ') || '(none)'}`
    )
    if (result.removed.length > 0) {
      console.log(`[afterPack] ASR resources removed: ${result.removed.join(', ')}`)
    }
  }
  if (platform === 'darwin') {
    adHocSignMacApp(context.appOutDir)
  }
}
