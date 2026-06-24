import { describe, expect, it } from 'vitest'
import {
  DEFAULT_REMOTE_IM_CONFIG,
  normalizeRemoteImConfig,
  validateRemoteImConfig
} from './config.js'

describe('remote IM config', () => {
  it('normalizes missing values to a disabled Tencent IM config', () => {
    expect(normalizeRemoteImConfig(undefined)).toEqual(DEFAULT_REMOTE_IM_CONFIG)
  })

  it('trims user ids and removes empty whitelist entries', () => {
    expect(
      normalizeRemoteImConfig({
        enabled: true,
        provider: 'tencent-im',
        sdkAppId: '1400000000',
        desktopUserId: ' desktop_bot ',
        userSigEndpoint: ' https://example.test/sig ',
        allowedUserIds: [' phone_admin ', '', 'phone_admin'],
        outputFlushIntervalMs: 500,
        outputMaxChunkChars: 10
      })
    ).toMatchObject({
      enabled: true,
      provider: 'tencent-im',
      sdkAppId: 1400000000,
      desktopUserId: 'desktop_bot',
      userSigEndpoint: 'https://example.test/sig',
      allowedUserIds: ['phone_admin'],
      outputFlushIntervalMs: 1000,
      outputMaxChunkChars: 200
    })
  })

  it('rejects enabled configs without required Tencent IM fields', () => {
    const result = validateRemoteImConfig({
      ...DEFAULT_REMOTE_IM_CONFIG,
      enabled: true
    })
    expect(result.ok).toBe(false)
    expect(result.issues.map((issue) => issue.path)).toEqual([
      'sdkAppId',
      'desktopUserId',
      'userSigEndpoint',
      'allowedUserIds'
    ])
  })
})
