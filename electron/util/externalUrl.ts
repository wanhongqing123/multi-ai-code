export function normalizeExternalHttpUrl(value: string): string | null {
  const candidate = value.trim()
  if (!candidate) return null

  try {
    const url = new URL(candidate)
    if (url.protocol !== 'http:' && url.protocol !== 'https:') return null
    return url.href
  } catch {
    return null
  }
}
