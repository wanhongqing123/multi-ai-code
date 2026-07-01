function isMissingCommandError(error) {
  return error?.code === 'ENOENT' || String(error?.message ?? '').includes('ENOENT')
}

export async function extractZipArchive(zipPath, destDir, deps) {
  try {
    await deps.run('unzip', ['-q', zipPath, '-d', destDir])
  } catch (error) {
    if (!isMissingCommandError(error)) throw error
    await deps.run('tar', ['-xf', zipPath, '-C', destDir])
  }
}
