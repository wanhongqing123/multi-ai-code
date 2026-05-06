function detectSeparator(root: string): '/' | '\\' {
  return root.includes('\\') && !root.includes('/') ? '\\' : '/'
}

function trimTrailingSeparators(root: string): string {
  return root.replace(/[\\/]+$/, '')
}

export function joinWithRootStyle(root: string, ...parts: string[]): string {
  const sep = detectSeparator(root)
  const cleanedRoot = trimTrailingSeparators(root)
  const cleanedParts = parts
    .flatMap((part) => part.split(/[\\/]+/))
    .map((part) => part.trim())
    .filter(Boolean)

  if (cleanedParts.length === 0) return cleanedRoot
  return `${cleanedRoot}${sep}${cleanedParts.join(sep)}`
}
