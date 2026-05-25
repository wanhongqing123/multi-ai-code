import { describe, expect, it } from 'vitest'
import {
  KB_SCHEDULER_DEFAULTS,
  decideKbRun,
  describeSkipReason,
  type KbSchedulerSignals
} from './scheduler.js'

const NOW = 1_700_000_000_000
const D = KB_SCHEDULER_DEFAULTS

function signals(over: Partial<KbSchedulerSignals> = {}): KbSchedulerSignals {
  return {
    lastSummaryAt: 0,
    lastAiActivityAt: 0,
    lastUserPromptAt: 0,
    pendingSignalCount: 0,
    mainSessionRunning: true,
    cliConfigured: true,
    ...over
  }
}

describe('decideKbRun: hard skips', () => {
  it('skips when main session is not running', () => {
    const v = decideKbRun(
      signals({ mainSessionRunning: false, pendingSignalCount: 50 }),
      NOW
    )
    expect(v).toEqual({ run: false, reason: 'main-session-not-running' })
  })

  it('skips when no CLI is configured', () => {
    const v = decideKbRun(
      signals({ cliConfigured: false, pendingSignalCount: 50 }),
      NOW
    )
    expect(v).toEqual({ run: false, reason: 'no-cli-config' })
  })

  it('skips when AI is mid-stream (recent cc:data)', () => {
    const v = decideKbRun(
      signals({
        lastAiActivityAt: NOW - 2000,
        pendingSignalCount: 50,
        lastUserPromptAt: NOW - 60 * 60 * 1000
      }),
      NOW
    )
    expect(v.run).toBe(false)
    if (!v.run) expect(v.reason).toBe('ai-streaming')
  })

  it('skips when the user just submitted a prompt', () => {
    const v = decideKbRun(
      signals({
        lastUserPromptAt: NOW - 5000,
        pendingSignalCount: 50
      }),
      NOW
    )
    expect(v.run).toBe(false)
    if (!v.run) expect(v.reason).toBe('user-just-prompted')
  })

  it('skips when the last summary is too recent (rate limit)', () => {
    const v = decideKbRun(
      signals({
        lastSummaryAt: NOW - 10 * 60 * 1000,
        pendingSignalCount: 50
      }),
      NOW
    )
    expect(v.run).toBe(false)
    if (!v.run) expect(v.reason).toBe('rate-limited')
  })

  it('skips when there are zero new signals', () => {
    const v = decideKbRun(
      signals({
        lastSummaryAt: NOW - 5 * 60 * 60 * 1000,
        pendingSignalCount: 0
      }),
      NOW
    )
    expect(v.run).toBe(false)
    if (!v.run) expect(v.reason).toBe('no-new-signal')
  })
})

describe('decideKbRun: positive triggers', () => {
  it('fires "long-overdue" when 24h elapsed and any signal exists', () => {
    const v = decideKbRun(
      signals({
        lastSummaryAt: NOW - D.longOverdueMs - 1000,
        pendingSignalCount: 1,
        lastUserPromptAt: NOW - 60 * 1000
      }),
      NOW
    )
    expect(v).toEqual({ run: true, reason: 'long-overdue' })
  })

  it('fires "signal-threshold" when enough pending prompts accumulate', () => {
    const v = decideKbRun(
      signals({
        lastSummaryAt: NOW - 2 * 60 * 60 * 1000,
        pendingSignalCount: D.signalThreshold,
        lastUserPromptAt: NOW - 60 * 1000
      }),
      NOW
    )
    expect(v).toEqual({ run: true, reason: 'signal-threshold' })
  })

  it('fires "idle-window" when user idle long enough and any signal exists', () => {
    const v = decideKbRun(
      signals({
        lastSummaryAt: NOW - 2 * 60 * 60 * 1000,
        pendingSignalCount: 2,
        lastUserPromptAt: NOW - D.idleWindowMs - 1000
      }),
      NOW
    )
    expect(v).toEqual({ run: true, reason: 'idle-window' })
  })

  it('long-overdue beats signal-threshold (long-overdue takes priority)', () => {
    const v = decideKbRun(
      signals({
        lastSummaryAt: NOW - D.longOverdueMs - 1000,
        pendingSignalCount: D.signalThreshold + 10,
        lastUserPromptAt: NOW - 60 * 1000
      }),
      NOW
    )
    if (v.run) expect(v.reason).toBe('long-overdue')
  })
})

describe('decideKbRun: edge cases', () => {
  it('handles a fresh install (lastSummaryAt = 0) without rate-limiting itself', () => {
    const v = decideKbRun(
      signals({
        lastSummaryAt: 0,
        pendingSignalCount: D.signalThreshold,
        lastUserPromptAt: NOW - 60 * 1000
      }),
      NOW
    )
    expect(v.run).toBe(true)
  })

  it('returns thresholds-not-met when signals are below threshold and user is busy', () => {
    const v = decideKbRun(
      signals({
        lastSummaryAt: NOW - 2 * 60 * 60 * 1000,
        pendingSignalCount: 2,
        lastUserPromptAt: NOW - 2 * 60 * 1000
      }),
      NOW
    )
    expect(v).toEqual({ run: false, reason: 'thresholds-not-met' })
  })

  it('respects custom thresholds for tuning', () => {
    const tighter = { ...D, signalThreshold: 1 }
    const v = decideKbRun(
      signals({
        lastSummaryAt: NOW - 2 * 60 * 60 * 1000,
        pendingSignalCount: 1,
        lastUserPromptAt: NOW - 60 * 1000
      }),
      NOW,
      tighter
    )
    expect(v.run).toBe(true)
  })
})

describe('describeSkipReason', () => {
  it('returns a non-empty string for every skip reason', () => {
    const reasons = [
      'main-session-not-running',
      'no-cli-config',
      'ai-streaming',
      'user-just-prompted',
      'rate-limited',
      'no-new-signal',
      'thresholds-not-met'
    ] as const
    for (const r of reasons) {
      expect(describeSkipReason(r).length).toBeGreaterThan(0)
    }
  })
})
