import { describe, expect, it } from 'vitest'
import type { RemoteImStatus } from './types.js'
import {
  createRemoteImAccountChangedStatuses,
  getRemoteImSendConnectionError
} from './status.js'

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
        detail: 'Remote IM account changed; waiting for reconnect',
        updatedAt: 1234
      }
    ])
  })

  it('keeps disabled projects disabled when the active IM account changes', () => {
    expect(
      createRemoteImAccountChangedStatuses(
        [{ ...connectedStatus, state: 'disabled', detail: null }],
        1234
      )
    ).toEqual([
      {
        projectId: 'project-1',
        state: 'disabled',
        detail: null,
        updatedAt: 1234
      }
    ])
  })

  it('blocks manual peer sends until the current project is connected', () => {
    expect(getRemoteImSendConnectionError(connectedStatus)).toBeNull()
    expect(getRemoteImSendConnectionError({ ...connectedStatus, state: 'connecting' })).toBe(
      'Remote IM is not connected'
    )
    expect(
      getRemoteImSendConnectionError({
        ...connectedStatus,
        state: 'error',
        detail: 'Tencent IM login failed'
      })
    ).toBe('Remote IM is not connected: Tencent IM login failed')
  })
})
