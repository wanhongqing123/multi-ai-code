import React from 'react'
import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it } from 'vitest'
import RemoteDesktopSettingsCard from '../../../src/components/RemoteDesktopSettingsCard.js'
import type { RemoteDesktopMode } from '../../../electron/remote-im/types.js'

function markup(
  mode: RemoteDesktopMode,
  allowedUserIds: string[] = ['whq-iphone'],
  control = false
): string {
  return renderToStaticMarkup(
    React.createElement(RemoteDesktopSettingsCard, {
      mode,
      allowedUserIds,
      onMode: () => {},
      control,
      onControl: () => {}
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

  it('keeps control behind its own opt-in, off by default', () => {
    // 开"看屏幕"不等于把整台电脑交出去：控制必须再单独授权一次。
    // 只盯 checkbox：整段 markup 里被选中的模式单选钮也带 checked。
    expect(markup('unattended')).toContain('允许对方操作我的键盘和鼠标')
    expect(markup('unattended')).toContain('<input type="checkbox"/>')
    expect(markup('unattended', ['whq-iphone'], true)).toContain(
      '<input type="checkbox" checked=""/>'
    )
  })

  it('does not offer control while remote desktop is off', () => {
    // 整个功能关着的时候摆一个"允许操作"的勾选框只会让人困惑。
    expect(markup('disabled')).not.toContain('允许对方操作我的键盘和鼠标')
  })

  it('tells the user Win+L is blocked', () => {
    // 锁屏是不可逆的：锁上之后只能本人到电脑前解。这条必须写在界面上。
    expect(markup('unattended')).toContain('Win+L')
  })

  it('mentions the indicator bar only when the feature is on', () => {
    // 关闭状态下讲指示条只会让人困惑。
    expect(markup('disabled')).not.toContain('常驻红色提示条')
    expect(markup('unattended')).toContain('常驻红色提示条')
  })
})
