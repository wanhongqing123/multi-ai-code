import { join } from 'node:path'
import { describe, expect, it } from 'vitest'
import {
  getRemoteImAccountProfileId,
  getRemoteImProfileId,
  getRemoteImRuntimeProfileId,
  resolveRemoteImUserDataPath
} from './profile.js'

describe('remote IM profile', () => {
  it('reads the profile id from command line args before environment variables', () => {
    expect(
      getRemoteImProfileId(
        ['electron', '.', '--multi-ai-profile=test123'],
        { MULTI_AI_PROFILE: 'test321' }
      )
    ).toBe('test123')
  })

  it('uses the MULTI_AI_PROFILE environment variable when no arg is provided', () => {
    expect(getRemoteImProfileId(['electron', '.'], { MULTI_AI_PROFILE: 'test321' })).toBe(
      'test321'
    )
  })

  it('rejects profile ids that are not safe Tencent IM UserID-style identifiers', () => {
    expect(getRemoteImProfileId(['electron', '.', '--multi-ai-profile=test 123'], {})).toBeNull()
    expect(getRemoteImProfileId(['electron', '.', '--multi-ai-profile=../test123'], {})).toBeNull()
    expect(getRemoteImProfileId(['electron', '.', '--multi-ai-profile='], {})).toBeNull()
  })

  it('resolves profile-specific Electron userData under the default userData root', () => {
    expect(resolveRemoteImUserDataPath('C:\\Users\\me\\AppData\\Roaming\\multi-ai-code', 'test123')).toBe(
      join('C:\\Users\\me\\AppData\\Roaming\\multi-ai-code', 'profiles', 'test123')
    )
  })

  it('creates an isolated runtime profile for ordinary double-launches', () => {
    expect(getRemoteImRuntimeProfileId(['electron', '.'], {}, 12345)).toBe('instance-12345')
  })

  it('uses the explicit profile as the runtime profile when provided', () => {
    expect(
      getRemoteImRuntimeProfileId(
        ['electron', '.', '--multi-ai-profile=test123'],
        { MULTI_AI_PROFILE: 'test321' },
        12345
      )
    ).toBe('test123')
  })

  it('uses a safe UserID as the account profile id after login', () => {
    expect(getRemoteImAccountProfileId(' test321 ')).toBe('test321')
    expect(getRemoteImAccountProfileId('../test321')).toBeNull()
  })
})
