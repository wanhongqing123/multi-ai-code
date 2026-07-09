export const OPENCODE_LSP_CONFIG_CONTENT = JSON.stringify({
  $schema: 'https://opencode.ai/config.json',
  lsp: true
})

export interface OpenCodeProviderProfile {
  providerId?: string
  name?: string
  baseURL?: string
  apiKeyEnvVar?: string
  mainModel?: string
  smallModel?: string
  timeoutMs?: number
  chunkTimeoutMs?: number
}

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

function normalizeProviderProfile(
  profile: OpenCodeProviderProfile | undefined
): Required<Pick<OpenCodeProviderProfile, 'providerId' | 'name' | 'baseURL' | 'mainModel'>> &
  Pick<OpenCodeProviderProfile, 'apiKeyEnvVar' | 'smallModel' | 'timeoutMs' | 'chunkTimeoutMs'> | null {
  const providerId = profile?.providerId?.trim()
  const baseURL = profile?.baseURL?.trim()
  const mainModel = profile?.mainModel?.trim()
  if (!providerId || !baseURL || !mainModel) return null
  return {
    providerId,
    name: profile?.name?.trim() || providerId,
    baseURL,
    mainModel,
    apiKeyEnvVar: profile?.apiKeyEnvVar?.trim() || undefined,
    smallModel: profile?.smallModel?.trim() || undefined,
    timeoutMs: profile?.timeoutMs,
    chunkTimeoutMs: profile?.chunkTimeoutMs
  }
}

function parseOpenCodeConfigContent(content: string): Record<string, unknown> | null {
  let parsed: unknown
  try {
    parsed = JSON.parse(content)
  } catch {
    return null
  }
  if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) return null
  return parsed as Record<string, unknown>
}

function buildProviderConfig(profile: NonNullable<ReturnType<typeof normalizeProviderProfile>>) {
  const models: Record<string, { name: string }> = {
    [profile.mainModel]: { name: profile.mainModel }
  }
  const smallModel = profile.smallModel || profile.mainModel
  models[smallModel] = { name: smallModel }

  const options: Record<string, unknown> = {
    baseURL: profile.baseURL
  }
  if (profile.apiKeyEnvVar) options.apiKey = `{env:${profile.apiKeyEnvVar}}`
  if (typeof profile.timeoutMs === 'number' && Number.isFinite(profile.timeoutMs)) {
    options.timeout = profile.timeoutMs
  }
  if (typeof profile.chunkTimeoutMs === 'number' && Number.isFinite(profile.chunkTimeoutMs)) {
    options.chunkTimeout = profile.chunkTimeoutMs
  }

  return {
    npm: '@ai-sdk/openai-compatible',
    name: profile.name,
    options,
    models
  }
}

function mergeOpenCodeConfigContent(
  content: string,
  profile?: OpenCodeProviderProfile
): string | null {
  const parsed = parseOpenCodeConfigContent(content)
  if (!parsed) return null

  const normalizedProfile = normalizeProviderProfile(profile)
  if (!normalizedProfile && Object.prototype.hasOwnProperty.call(parsed, 'lsp')) return content

  const next: Record<string, unknown> = {
    $schema: 'https://opencode.ai/config.json',
    ...parsed,
    lsp: true
  }

  if (normalizedProfile) {
    const provider =
      parsed.provider && typeof parsed.provider === 'object' && !Array.isArray(parsed.provider)
        ? { ...(parsed.provider as Record<string, unknown>) }
        : {}
    provider[normalizedProfile.providerId] = buildProviderConfig(normalizedProfile)
    next.provider = provider
    next.model = `${normalizedProfile.providerId}/${normalizedProfile.mainModel}`
    next.small_model = `${normalizedProfile.providerId}/${
      normalizedProfile.smallModel || normalizedProfile.mainModel
    }`
    if (!Object.prototype.hasOwnProperty.call(parsed, 'autoupdate')) {
      next.autoupdate = false
    }
  }

  return JSON.stringify(next)
}

export function withOpenCodeLspEnv(
  command: string,
  env: Record<string, string> | undefined,
  profile?: OpenCodeProviderProfile
): Record<string, string> | undefined {
  if (!isOpenCodeCommand(command)) return env
  const next = { ...(env ?? {}) }
  const existing = next.OPENCODE_CONFIG_CONTENT
  if (!existing) {
    next.OPENCODE_CONFIG_CONTENT =
      mergeOpenCodeConfigContent(OPENCODE_LSP_CONFIG_CONTENT, profile) ??
      OPENCODE_LSP_CONFIG_CONTENT
    return next
  }
  const merged = mergeOpenCodeConfigContent(existing, profile)
  if (merged) next.OPENCODE_CONFIG_CONTENT = merged
  return next
}
