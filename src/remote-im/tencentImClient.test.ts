import { describe, expect, it } from 'vitest'
import { extractTencentImTextMessages, extractUserSig } from './tencentImClient.js'

describe('tencent IM client helpers', () => {
  it('extracts UserSig from supported endpoint response shapes', () => {
    expect(extractUserSig({ userSig: 'sig-1' })).toBe('sig-1')
    expect(extractUserSig({ ok: true, userSig: 'sig-2' })).toBe('sig-2')
    expect(() => extractUserSig({ ok: false })).toThrow('UserSig')
  })

  it('extracts C2C text messages from Tencent message events', () => {
    const messages = extractTencentImTextMessages({
      data: [
        {
          ID: 'msg-1',
          from: 'phone_admin',
          to: 'desktop_bot',
          type: 'TIMTextElem',
          payload: { text: 'hello' },
          time: 1782238800
        },
        {
          ID: 'msg-2',
          from: 'phone_admin',
          to: 'desktop_bot',
          type: 'TIMImageElem',
          payload: {}
        }
      ]
    })

    expect(messages).toEqual([
      {
        remoteMessageId: 'msg-1',
        fromUserId: 'phone_admin',
        toUserId: 'desktop_bot',
        text: 'hello',
        createdAt: 1782238800000
      }
    ])
  })
})
