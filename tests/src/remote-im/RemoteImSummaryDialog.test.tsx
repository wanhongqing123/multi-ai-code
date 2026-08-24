import React from 'react'
import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it } from 'vitest'
import type { RemoteImMessage } from '../../../electron/preload.js'
import {
  RemoteImSummaryImage,
  isScrolledToBottom
} from '../../../src/remote-im/RemoteImSummaryDialog.js'

function imageMessage(overrides: Partial<RemoteImMessage> = {}): RemoteImMessage {
  return {
    id: 64,
    projectId: 'project-1',
    sessionId: null,
    provider: 'tencent-im',
    remoteMessageId: 'remote-image-1',
    fromUserId: 'whq-iphone',
    toUserId: 'desktop',
    role: 'remote-user',
    direction: 'incoming',
    content: '[图片消息] shot.png',
    kind: 'image',
    attachment: {
      type: 'image',
      localPath: '/Users/test/remote-im/images/project-1/shot.png',
      remoteUrl: null,
      thumbnailUrl: null,
      width: 1290,
      height: 2796,
      sizeBytes: 4096,
      fileName: 'shot.png',
      mimeType: 'image/png',
      sdkImageId: null
    },
    status: 'received',
    error: null,
    createdAt: 1,
    sentToAicliAt: null,
    sentToImAt: null,
    ...overrides
  }
}

describe('RemoteImSummaryImage', () => {
  it('uses the controlled preview loader for local history images', () => {
    const markup = renderToStaticMarkup(
      <RemoteImSummaryImage projectId="project-1" message={imageMessage()} />
    )

    expect(markup).toContain('remote-im-summary-image-state loading')
    expect(markup).toContain('正在读取 shot.png')
    expect(markup).not.toContain('/Users/test/remote-im/images')
  })

  it('renders a remote thumbnail when no local cache is available', () => {
    const original = imageMessage()
    if (original.attachment?.type !== 'image') throw new Error('expected image attachment')
    const message = imageMessage({
      attachment: {
        ...original.attachment,
        localPath: null,
        thumbnailUrl: 'https://example.test/shot-thumb.png'
      }
    })
    const markup = renderToStaticMarkup(
      <RemoteImSummaryImage projectId="project-1" message={message} />
    )

    expect(markup).toContain('class="remote-im-summary-image-preview"')
    expect(markup).toContain('src="https://example.test/shot-thumb.png"')
    expect(markup).toContain('alt="shot.png"')
  })
})

// 时光胶囊打开时要停在最后一条。持续贴底的开关就靠这个判断：判反了要么自动定位
// 失效，要么用户往回翻会被一直拽回底部——两种都不会报错，只能靠断言守住。
describe('isScrolledToBottom', () => {
  it('恰好在底部时算贴底', () => {
    expect(isScrolledToBottom({ scrollTop: 900, scrollHeight: 1400, clientHeight: 500 })).toBe(true)
  })

  it('差几像素仍算贴底，避免图片加载的抖动误判为用户滚动', () => {
    expect(isScrolledToBottom({ scrollTop: 897, scrollHeight: 1400, clientHeight: 500 })).toBe(true)
  })

  it('用户往回翻之后不再算贴底', () => {
    expect(isScrolledToBottom({ scrollTop: 200, scrollHeight: 1400, clientHeight: 500 })).toBe(false)
  })

  it('内容不足一屏时算贴底，此时没有可滚动空间', () => {
    expect(isScrolledToBottom({ scrollTop: 0, scrollHeight: 300, clientHeight: 500 })).toBe(true)
  })
})
