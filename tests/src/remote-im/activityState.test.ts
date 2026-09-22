import { afterEach, expect, it, vi } from 'vitest'
import { activityKey, receiveActivity, getActivity, clearActivity, clearAccountActivities } from '../../../src/remote-im/activityState'

afterEach(() => { clearAccountActivities('owner'); vi.useRealTimers() })
it('ignores reordered stops and heartbeats, expires without touching history', () => {
  vi.useFakeTimers()
  const key = activityKey('owner', 'peer')
  const signal = { activityId: 'id-one', sequence: 1, kind: 'machine-working' as const, active: true, ttlMs: 12000 }
  receiveActivity(key, signal)
  receiveActivity(key, { ...signal, sequence: 3, kind: 'machine-tool' })
  receiveActivity(key, { ...signal, sequence: 2 })
  expect(getActivity(key)?.kind).toBe('machine-tool')
  receiveActivity(key, { ...signal, activityId: 'older', active: false })
  expect(getActivity(key)?.activityId).toBe('id-one')
  vi.advanceTimersByTime(12001)
  expect(getActivity(key)).toBeUndefined()
  receiveActivity(key, { ...signal, sequence: 4 })
  expect(getActivity(key)?.activityId).toBe('id-one')
})
it('content closes the old activity and a later machine phase gets a new identity', () => {
  const key = activityKey('owner', 'other-peer')
  const signal = { activityId: 'id-two', sequence: 1, kind: 'human-typing' as const, active: true, ttlMs: 12000 }
  receiveActivity(key, signal)
  clearActivity(key)
  receiveActivity(key, { ...signal, sequence: 2 })
  expect(getActivity(key)).toBeUndefined()
  receiveActivity(key, { ...signal, activityId: 'id-three', kind: 'machine-thinking' })
  expect(getActivity(key)?.kind).toBe('machine-thinking')
  expect(getActivity(activityKey('another-owner', 'other-peer'))).toBeUndefined()
})
