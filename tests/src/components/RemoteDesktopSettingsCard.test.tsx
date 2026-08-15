import React from 'react'
import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it } from 'vitest'
import RemoteDesktopSettingsCard from '../../../src/components/RemoteDesktopSettingsCard.js'
import type { RemoteDesktopMode } from '../../../electron/remote-im/types.js'

function markup(mode: RemoteDesktopMode, allowedUserIds: string[] = ['whq-iphone']): string {
  return renderToStaticMarkup(
    React.createElement(RemoteDesktopSettingsCard, {
      mode,
      allowedUserIds,
      onMode: () => {}
    })
  )
}

describe('remote desktop settings card', () => {
  it('offers exactly the three supported modes', () => {
    const html = markup('disabled')
    expect(html).toContain('关闭')
    expect(html).toContain('无人值守')
    expect(html).toContain('每次确认')
    expect((html.match(/type="radio"/g) ?? [])).toHaveLength(3)
  })

  it('marks the current mode as selected', () => {
    expect(markup('unattended')).toContain('checked=""')
    // 三个选项里只能有一个被选中。
    expect((markup('unattended').match(/checked=""/g) ?? [])).toHaveLength(1)
  })

  it('states that this machine can only be viewed, never view others', () => {
    // 只能被控是代码层面的事实，界面必须说清楚，否则用户会去找"连接对方"的入口。
    expect(markup('disabled')).toContain('本机只作被控端')
    expect(markup('disabled')).not.toContain('发起')
  })

  it('warns when the allow list is empty', () => {
    // 白名单为空时开启也没用。不明说的话用户会以为开了就能连。
    const html = markup('unattended', [])
    expect(html).toContain('当前没有任何设备能连入')
  })

  it('lists the devices that may connect', () => {
    expect(markup('unattended', ['whq-iphone', 'mac-a'])).toContain('whq-iphone、mac-a')
  })

  it('mentions the indicator bar only when the feature is on', () => {
    // 关闭状态下讲指示条只会让人困惑。
    expect(markup('disabled')).not.toContain('常驻红色提示条')
    expect(markup('unattended')).toContain('常驻红色提示条')
  })
})
