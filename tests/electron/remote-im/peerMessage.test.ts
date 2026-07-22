import { describe, expect, it } from 'vitest'
import type { RemoteImConfig } from '../../../electron/remote-im/types.js'
import {
  createPeerOutgoingImageMessageInput,
  createPeerOutgoingMessageInput,
  resolvePeerUserId
} from '../../../electron/remote-im/peerMessage.js'

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
  it('uses the first trusted friend UserID as the default peer', () => {
    expect(resolvePeerUserId(config)).toBe('friend-a')
    expect(resolvePeerUserId(config, 'friend-a')).toBe('friend-a')
    expect(resolvePeerUserId({ ...config, friendUserIds: [], masterUserIds: [], slaveUserIds: [], allowedUserIds: [] })).toBeNull()
    expect(resolvePeerUserId({ ...config, desktopRole: 'slave' })).toBe('friend-a')
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

  it('creates an outgoing image message record for a peer IM send', () => {
    expect(
      createPeerOutgoingImageMessageInput({
        projectId: 'project-1',
        config,
        toUserId: 'desktop-b',
        now: 1234,
        attachment: {
          type: 'image',
          localPath: '/tmp/photo.png',
          remoteUrl: null,
          thumbnailUrl: null,
          width: null,
          height: null,
          sizeBytes: 1024,
          fileName: 'photo.png',
          mimeType: 'image/png',
          sdkImageId: null
        }
      })
    ).toMatchObject({
      projectId: 'project-1',
      fromUserId: 'desktop-a',
      toUserId: 'desktop-b',
      direction: 'outgoing',
      content: '[图片消息] photo.png',
      kind: 'image',
      attachment: {
        type: 'image',
        localPath: '/tmp/photo.png'
      },
      status: 'streaming'
    })
  })
})
