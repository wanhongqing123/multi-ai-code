import { beforeEach, describe, expect, it } from 'vitest'
import {
  MIN_RECORD_TEXT_LENGTH,
  clearRecordHabitDedupeForTests,
  shouldRecordText
} from './collector.js'

beforeEach(() => {
  clearRecordHabitDedupeForTests()
})

describe('shouldRecordText: length threshold', () => {
  it(`rejects text below ${MIN_RECORD_TEXT_LENGTH} characters`, () => {
    expect(shouldRecordText('short')).toBe(false)
    expect(shouldRecordText('   pad   ')).toBe(false)
  })

  it('accepts text at or above the threshold', () => {
    expect(shouldRecordText('this is long enough')).toBe(true)
  })

  it('treats whitespace-trimmed length, not raw length', () => {
    const padded = '   ' + 'x'.repeat(MIN_RECORD_TEXT_LENGTH - 1) + '   '
    expect(shouldRecordText(padded)).toBe(false)
  })
})

describe('shouldRecordText: 24h dedupe window', () => {
  it('records the same text only once per 24h window', () => {
    const t0 = 1_700_000_000_000
    expect(shouldRecordText('git status --short', t0)).toBe(true)
    expect(shouldRecordText('git status --short', t0 + 60_000)).toBe(false)
  })

  it('records the same text again after 24h', () => {
    const t0 = 1_700_000_000_000
    expect(shouldRecordText('git status --short', t0)).toBe(true)
    const dayLater = t0 + 24 * 60 * 60 * 1000 + 1
    expect(shouldRecordText('git status --short', dayLater)).toBe(true)
  })

  it('treats different texts as independent', () => {
    const t0 = 1_700_000_000_000
    expect(shouldRecordText('git status --short', t0)).toBe(true)
    expect(shouldRecordText('git diff --stat', t0)).toBe(true)
  })

  it('uses trimmed text for dedupe key, so leading/trailing whitespace is irrelevant', () => {
    const t0 = 1_700_000_000_000
    expect(shouldRecordText('npm run dev', t0)).toBe(true)
    expect(shouldRecordText('   npm run dev   ', t0 + 1000)).toBe(false)
  })
})
