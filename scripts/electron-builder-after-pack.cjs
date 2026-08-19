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

function adHocSignMacApp(appOutDir, arch) {
  const appBundle = findMacAppBundle(appOutDir)
  if (!appBundle) {
    console.log('[afterPack] macOS app bundle not found, skip ad-hoc codesign')
    return null
  }
  try {
    execFileSync('xattr', ['-cr', appBundle], { stdio: 'inherit' })
  } catch {
    // xattr is best-effort; codesign below is the important verification step.
  }
  // Bun emits OpenCode with a linker ad-hoc signature that can become stale after
  // compilation. `codesign --deep` on the outer app does not repair that nested
  // executable, so sign and verify it explicitly before sealing the app bundle.
  const openCodeBinary = join(
    appBundle,
    'Contents',
    'Resources',
    'app.asar.unpacked',
    'bin',
    'aicli',
    'opencode',
    `darwin-${arch}`,
    'opencode'
  )
  if (!existsSync(openCodeBinary)) {
    throw new Error(`[afterPack] OpenCode binary not found: ${openCodeBinary}`)
  }
  execFileSync('codesign', ['--force', '--sign', '-', openCodeBinary], {
    stdio: 'inherit'
  })
  execFileSync('codesign', ['--verify', '--strict', '--verbose=2', openCodeBinary], {
    stdio: 'inherit'
  })
  execFileSync('codesign', ['--force', '--deep', '--sign', '-', appBundle], {
    stdio: 'inherit'
  })
  execFileSync('codesign', ['--verify', '--deep', '--strict', '--verbose=2', appBundle], {
    stdio: 'inherit'
  })
  console.log(`[afterPack] macOS app ad-hoc signed: ${appBundle}`)
  return appBundle
}

module.exports = async function afterPack(context) {
  const { verifyPackagedCodexBinaries } = await import('./aicli-packaging.mjs')
  const platform = context.electronPlatformName
  const arch = archNameFromElectronBuilder(context.arch)
  const codexBinaries = verifyPackagedCodexBinaries(context.appOutDir, { platform, arch })
  console.log(`[afterPack] Codex binaries verified: ${codexBinaries.join(', ')}`)
  if (platform === 'darwin') {
    const appBundle = adHocSignMacApp(context.appOutDir, arch)
    if (!appBundle) throw new Error('[afterPack] macOS app bundle is required')
    const { verifyPackagedMacNativeModules } = await import(
      './verify-packaged-native-modules.mjs'
    )
    const nativeSummary = verifyPackagedMacNativeModules(appBundle)
    console.log(
      `[afterPack] Electron ABI ${nativeSummary.abi}; better-sqlite3 ${nativeSummary.sqliteVersion}; node-pty loaded`
    )
  }
  if (platform === 'win32') {
    // Windows 比 macOS 多两个原生模块：koffi（远程控制注入）和 TRTC SDK
    // （远程桌面）。koffi 的二进制还在平台专属的 @koromix/koffi-win32-x64
    // 里，不在 koffi 包内——只靠人工记得去验不可靠，必须进自动关卡。
    const { verifyPackagedWindowsNativeModules } = await import(
      './verify-packaged-native-modules.mjs'
    )
    const nativeSummary = verifyPackagedWindowsNativeModules(context.appOutDir)
    const extras = Object.entries(nativeSummary.loaded ?? {})
      .map(([name, detail]) => `${name} ${detail}`)
      .join('; ')
    console.log(`[afterPack] Electron ABI ${nativeSummary.abi}; ${extras}`)
  }
}
