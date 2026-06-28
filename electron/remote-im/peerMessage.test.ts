import { describe, expect, it } from 'vitest'
import type { RemoteImConfig } from './types.js'
import { createPeerOutgoingMessageInput, resolvePeerUserId } from './peerMessage.js'

const config: RemoteImConfig = {
  enabled: true,
  provider: 'tencent-im',
  sdkAppId: 1400704311,
  desktopUserId: 'desktop-a',
  desktopRole: 'master',
  userSigMode: 'secret-key',
  userSigEndpoint: '',
  userSigSecretKey: 'local-secret',
  friendUserIds: ['friend-a'],
  masterUserIds: ['desktop-b'],
  slaveUserIds: [],
  allowedUserIds: ['friend-a', 'desktop-b'],
  outputFlushIntervalMs: 2000,
  outputMaxChunkChars: 1200
}

describe('remote IM peer messages', () => {
  it('uses the first allowed UserID as the default peer', () => {
    expect(resolvePeerUserId(config)).toBe('desktop-b')
    expect(resolvePeerUserId(config, 'friend-a')).toBe('friend-a')
    expect(resolvePeerUserId({ ...config, friendUserIds: [], masterUserIds: [], slaveUserIds: [], allowedUserIds: [] })).toBeNull()
    expect(resolvePeerUserId({ ...config, desktopRole: 'slave' })).toBeNull()
  })

  it('creates an outgoing message record for a peer IM send', () => {
    expect(
      createPeerOutgoingMessageInput({
        projectId: 'project-1',
        config,
        toUserId: 'desktop-b',
        text: 'hello',
        now: 1234
      })
    ).toMatchObject({
      projectId: 'project-1',
      sessionId: null,
      provider: 'tencent-im',
      fromUserId: 'desktop-a',
      toUserId: 'desktop-b',
      role: 'remote-user',
      direction: 'outgoing',
      content: 'hello',
      status: 'streaming',
      sentToImAt: null
    })
  })
})
