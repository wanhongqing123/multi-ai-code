import React from 'react'
import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it } from 'vitest'
import RemoteDesktopSharingBar from '../../../src/remote-desktop/RemoteDesktopSharingBar.js'
import type { RemoteDesktopControllerState } from '../../../electron/remote-desktop/controller.js'

const sharing: RemoteDesktopControllerState = {
  hostState: 'sharing',
  peerUserId: 'whq-iphone',
  sessionId: 's-1'
}

function markup(state: RemoteDesktopControllerState): string {
  return renderToStaticMarkup(
    React.createElement(RemoteDesktopSharingBar, { state, onStop: () => {} })
  )
}

describe('remote desktop sharing bar', () => {
  it('names who is watching while sharing', () => {
    // 必须点名是谁在看：只说"正在共享"的话，你无从判断该不该停。
    const html = markup(sharing)
    expect(html).toContain('正在共享屏幕给 whq-iphone')
    expect(html).toContain('停止共享')
  })

  it('renders nothing when idle', () => {
    expect(markup({ hostState: 'idle', peerUserId: null, sessionId: null })).toBe('')
    expect(markup({ hostState: 'awaitingConsent', peerUserId: 'whq-iphone', sessionId: 's-1' })).toBe(
      ''
    )
  })

  it('offers no way to hide itself while sharing', () => {
    // 无人值守下这是自动放行之后唯一的可见性保障：给了隐藏入口，就等于允许
    // "屏幕正被看着，但界面上看不出来"。
    const buttons = markup(sharing).match(/<button/g) ?? []
    expect(buttons).toHaveLength(1)
    expect(markup(sharing)).not.toContain('隐藏')
  })

  it('exposes itself to assistive tech as a live status', () => {
    const html = markup(sharing)
    expect(html).toContain('role="status"')
    expect(html).toContain('aria-live="polite"')
  })
})
