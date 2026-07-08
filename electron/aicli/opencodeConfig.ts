export const OPENCODE_LSP_CONFIG_CONTENT = JSON.stringify({
  $schema: 'https://opencode.ai/config.json',
  lsp: true
})

function basenameLike(command: string): string {
  let normalized = command.trim()
  while (normalized.length >= 2) {
    const first = normalized[0]
    const last = normalized[normalized.length - 1]
    if ((first === '"' && last === '"') || (first === "'" && last === "'")) {
      normalized = normalized.slice(1, -1).trim()
      continue
    }
    break
  }
  const parts = normalized.split(/[\\/]+/)
  return (parts[parts.length - 1] ?? normalized).toLowerCase()
}

export function isOpenCodeCommand(command: string): boolean {
  return /^opencode(\.(exe|cmd|bat|ps1))?$/.test(basenameLike(command))
}

function mergeOpenCodeConfigContent(content: string): string | null {
  let parsed: unknown
  try {
    parsed = JSON.parse(content)
  } catch {
    return null
  }
  if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) return null
  if (Object.prototype.hasOwnProperty.call(parsed, 'lsp')) return content
  return JSON.stringify({
    $schema: 'https://opencode.ai/config.json',
    ...parsed,
    lsp: true
  })
}

export function withOpenCodeLspEnv(
  command: string,
  env: Record<string, string> | undefined
): Record<string, string> | undefined {
  if (!isOpenCodeCommand(command)) return env
  const next = { ...(env ?? {}) }
  const existing = next.OPENCODE_CONFIG_CONTENT
  if (!existing) {
    next.OPENCODE_CONFIG_CONTENT = OPENCODE_LSP_CONFIG_CONTENT
    return next
  }
  const merged = mergeOpenCodeConfigContent(existing)
  if (merged) next.OPENCODE_CONFIG_CONTENT = merged
  return next
}
