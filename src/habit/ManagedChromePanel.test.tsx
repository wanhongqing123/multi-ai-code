import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'
import type { ManagedChromeState } from '../../electron/preload'
import ManagedChromePanel, { getManagedChromeStatusLabel } from './ManagedChromePanel.js'

const stoppedState: ManagedChromeState = {
  running: false,
  port: null,
  profileDir: null,
  pid: null,
  lastActiveUrl: null
}

const runningState: ManagedChromeState = {
  running: true,
  port: 9222,
  profileDir: 'E:/tmp/managed-profile',
  pid: 4321,
  lastActiveUrl: 'https://docs.example.com/reference'
}

describe('ManagedChromePanel', () => {
  it('renders the stopped state with only the start action available', () => {
    const markup = renderToStaticMarkup(
      <ManagedChromePanel
        state={stoppedState}
        busy={false}
        onStart={vi.fn()}
        onFocus={vi.fn()}
        onStop={vi.fn()}
      />
    )

    expect(getManagedChromeStatusLabel(stoppedState)).toContain('未启动')
    expect(markup).toContain('托管 Chrome')
    expect(markup).toContain('未启动')
    expect(markup).toContain('启动')
    expect(markup).toContain('聚焦')
    expect(markup).toContain('停止')
  })

  it('renders the running state with port and last active url details', () => {
    const markup = renderToStaticMarkup(
      <ManagedChromePanel
        state={runningState}
        busy={false}
        onStart={vi.fn()}
        onFocus={vi.fn()}
        onStop={vi.fn()}
      />
    )

    expect(getManagedChromeStatusLabel(runningState)).toContain('9222')
    expect(markup).toContain('端口 9222')
    expect(markup).toContain('docs.example.com/reference')
  })
})
