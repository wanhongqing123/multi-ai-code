function archNameFromElectronBuilder(arch) {
  if (arch === 3 || arch === 'arm64') return 'arm64'
  if (arch === 0 || arch === 'ia32') return 'ia32'
  return 'x64'
}

module.exports = async function afterPack(context) {
  const { prunePackagedAsrResources } = await import('./asr-packaging.mjs')
  const platform = context.electronPlatformName
  const arch = archNameFromElectronBuilder(context.arch)
  const result = prunePackagedAsrResources(context.appOutDir, { platform, arch })
  if (result.missing) {
    console.log(`[afterPack] ASR resources not found for ${platform}-${arch}`)
    return
  }
  console.log(
    `[afterPack] ASR resources kept for ${platform}-${arch}: ${result.kept.join(', ') || '(none)'}`
  )
  if (result.removed.length > 0) {
    console.log(`[afterPack] ASR resources removed: ${result.removed.join(', ')}`)
  }
}
