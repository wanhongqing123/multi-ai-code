import { describe, expect, it } from 'vitest'
import type { RemoteImMessage } from '../../../electron/preload.js'
import { mergeRemoteImMessages } from '../../../src/remote-im/messageMerge.js'

function makeMessage(id: number, createdAt: number): RemoteImMessage {
  return {
    id,
    projectId: 'project-1',
    sessionId: null,
    provider: 'tencent-im',
    remoteMessageId: `remote-${id}`,
    fromUserId: 'phone_admin',
    toUserId: 'desktop_bot',
    role: 'remote-user',
    direction: 'incoming',
    content: `msg-${id}`,
    kind: 'text',
    attachment: null,
    status: 'received',
    error: null,
    createdAt,
    sentToAicliAt: null,
    sentToImAt: null
  }
}

describe('mergeRemoteImMessages', () => {
  it('prepends fetched history sorted by time and dedupes by id', () => {
    const existing = [makeMessage(3, 300), makeMessage(4, 400)]
    const fetched = [makeMessage(1, 100), makeMessage(2, 200), makeMessage(3, 300)]

    const merged = mergeRemoteImMessages(existing, fetched)

    expect(merged.map((message) => message.id)).toEqual([1, 2, 3, 4])
  })

  it('returns the existing array untouched when nothing new arrives', () => {
    const existing = [makeMessage(1, 100)]
    expect(mergeRemoteImMessages(existing, [])).toBe(existing)
    expect(mergeRemoteImMessages(existing, [makeMessage(1, 100)])).toBe(existing)
  })
})
