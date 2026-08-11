import React from 'react'
import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it } from 'vitest'
import type { RemoteImMessage } from '../../../electron/preload.js'
import { RemoteImSummaryImage } from '../../../src/remote-im/RemoteImSummaryDialog.js'

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
