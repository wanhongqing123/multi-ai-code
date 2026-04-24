import { describe, expect, it } from 'vitest'
import {
  canSendRepoAnnotations,
  repoSendButtonTitle
} from './analysisPanelState'

describe('canSendRepoAnnotations', () => {
  it('returns false when session is not running', () => {
    expect(canSendRepoAnnotations(false, 2)).toBe(false)
  })

  it('returns false when there are no annotations', () => {
    expect(canSendRepoAnnotations(true, 0)).toBe(false)
  })

  it('returns true only when session is running and annotations exist', () => {
    expect(canSendRepoAnnotations(true, 1)).toBe(true)
  })
})

describe('repoSendButtonTitle', () => {
  it('asks user to start cli first when session is stopped', () => {
    expect(repoSendButtonTitle(false, 3)).toBe('请先启动下方 AI CLI')
  })

  it('asks for at least one annotation when session is running but empty', () => {
    expect(repoSendButtonTitle(true, 0)).toBe('至少需要一条标注')
  })

  it('uses the injection hint when sending is allowed', () => {
    expect(repoSendButtonTitle(true, 2)).toBe('注入到下方 AI CLI')
  })
})
