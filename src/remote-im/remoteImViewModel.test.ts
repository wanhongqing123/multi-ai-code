import { describe, expect, it } from 'vitest'
import type { RemoteImMessage, RemoteImStatus } from '../../electron/preload.js'
import {
  getRemoteImMessageAvatar,
  getRemoteImStatusLabel,
  isRemoteImSendDisabled
} from './remoteImViewModel.js'

describe('remote IM view model', () => {
  it('maps connection status to short labels', () => {
    const status: RemoteImStatus = {
      projectId: 'project-1',
      state: 'connected',
      detail: null,
      updatedAt: 1
    }
    expect(getRemoteImStatusLabel(status)).toBe('已连接')
    expect(getRemoteImStatusLabel({ ...status, state: 'disabled' })).toBe('未开启')
    expect(getRemoteImStatusLabel({ ...status, state: 'error', detail: 'login failed' })).toBe(
      '异常'
    )
  })

  it('maps message roles to compact avatars', () => {
    const message = { role: 'remote-user' } as RemoteImMessage
    expect(getRemoteImMessageAvatar(message)).toBe('手')
    expect(getRemoteImMessageAvatar({ ...message, role: 'system' })).toBe('系')
    expect(getRemoteImMessageAvatar({ ...message, role: 'aicli' })).toBe('AI')
  })

  it('disables sending without project, text, or running session', () => {
    const status: RemoteImStatus = {
      projectId: 'project-1',
      state: 'connected',
      detail: null,
      updatedAt: 1
    }
    expect(isRemoteImSendDisabled({ projectId: null, sessionRunning: true, text: 'hi', status })).toBe(
      true
    )
    expect(isRemoteImSendDisabled({ projectId: 'project-1', sessionRunning: false, text: 'hi', status })).toBe(
      true
    )
    expect(isRemoteImSendDisabled({ projectId: 'project-1', sessionRunning: true, text: ' ', status })).toBe(
      true
    )
    expect(isRemoteImSendDisabled({ projectId: 'project-1', sessionRunning: true, text: 'hi', status })).toBe(
      false
    )
  })
})
