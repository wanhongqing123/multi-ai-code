import { join } from 'path'

const PROFILE_ARG = '--multi-ai-profile'
const SAFE_PROFILE_ID = /^[A-Za-z0-9_-]{1,32}$/

function normalizeProfileId(value: unknown): string | null {
  if (typeof value !== 'string') return null
  const profileId = value.trim()
  return SAFE_PROFILE_ID.test(profileId) ? profileId : null
}

export function getRemoteImProfileId(
  argv: string[] = process.argv,
  env: NodeJS.ProcessEnv = process.env
): string | null {
  for (let index = 0; index < argv.length; index += 1) {
    const arg = argv[index]
    if (arg === PROFILE_ARG) {
      const profileId = normalizeProfileId(argv[index + 1])
      if (profileId) return profileId
      return null
    }
    if (arg.startsWith(`${PROFILE_ARG}=`)) {
      return normalizeProfileId(arg.slice(PROFILE_ARG.length + 1))
    }
  }
  return normalizeProfileId(env.MULTI_AI_PROFILE)
}

export function getRemoteImRuntimeProfileId(
  argv: string[] = process.argv,
  env: NodeJS.ProcessEnv = process.env,
  pid: number = process.pid
): string {
  return getRemoteImProfileId(argv, env) ?? `instance-${pid}`
}

export function getRemoteImAccountProfileId(userId: string): string | null {
  return normalizeProfileId(userId)
}

export function resolveRemoteImUserDataPath(defaultUserDataPath: string, profileId: string): string {
  return join(defaultUserDataPath, 'profiles', profileId)
}
