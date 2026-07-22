import { describe, expect, it } from 'vitest'
import type { RemoteImStatus } from '../../../electron/remote-im/types.js'
import {
  createRemoteImAccountChangedStatuses,
  getRemoteImSendConnectionError
} from '../../../electron/remote-im/status.js'

const connectedStatus: RemoteImStatus = {
  projectId: 'project-1',
  state: 'connected',
  detail: null,
  updatedAt: 1
}

describe('remote IM status helpers', () => {
  it('resets connected project statuses after the active IM account changes', () => {
    expect(createRemoteImAccountChangedStatuses([connectedStatus], 1234)).toEqual([
      {
        projectId: 'project-1',
        state: 'disconnected',
        detail: '远程 IM 账号已变更，正在重新连接',
        updatedAt: 1234
      }
    ])
  })

  it('resets legacy disabled project statuses because IM no longer has an enabled switch', () => {
    expect(
      createRemoteImAccountChangedStatuses(
        [{ ...connectedStatus, state: 'disabled', detail: null }],
        1234
      )
    ).toEqual([
      {
        projectId: 'project-1',
        state: 'disconnected',
        detail: '远程 IM 账号已变更，正在重新连接',
        updatedAt: 1234
      }
    ])
  })

  it('blocks manual peer sends until the current project is connected', () => {
    expect(getRemoteImSendConnectionError(connectedStatus)).toBeNull()
    expect(getRemoteImSendConnectionError({ ...connectedStatus, state: 'connecting' })).toBe(
      '远程 IM 未连接'
    )
    expect(
      getRemoteImSendConnectionError({
        ...connectedStatus,
        state: 'error',
        detail: 'Tencent IM login failed'
      })
    ).toBe('远程 IM 未连接：IM 登录失败')
  })
})
