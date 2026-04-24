import { describe, expect, it } from 'vitest'
import {
  normalizeTerminalText,
  shouldAutoAcceptCodexTrustPrompt,
  isCodexReadyForPromptInjection,
  shouldAutoAcceptSessionEditPrompt
} from './codexTrust.js'

describe('normalizeTerminalText', () => {
  it('strips ansi escapes and normalizes whitespace', () => {
    const raw =
      '\u001b[1;1H> \u001b[1mYou are in \u001b[22m/tmp/demo\u001b[3;3HDo\u001b[3;6Hyou\u001b[3;10Htrust\u001b[3;16Hthe\u001b[3;20Hcontents\u001b[3;29Hof\u001b[3;32Hthis\u001b[3;37Hdirectory?\u001b[9;3H\u001b[2mPress enter to continue\u001b[m'
    expect(normalizeTerminalText(raw)).toContain('Do you trust the contents of this directory?')
    expect(normalizeTerminalText(raw)).toContain('Press enter to continue')
  })
})

describe('shouldAutoAcceptCodexTrustPrompt', () => {
  it('returns true when codex trust prompt is present', () => {
    const raw =
      '\u001b[3;3HDo\u001b[3;6Hyou\u001b[3;10Htrust\u001b[3;16Hthe\u001b[3;20Hcontents\u001b[3;29Hof\u001b[3;32Hthis\u001b[3;37Hdirectory?\u001b[9;3H\u001b[2mPress enter to continue\u001b[m'
    expect(shouldAutoAcceptCodexTrustPrompt(raw)).toBe(true)
  })

  it('returns false for unrelated output', () => {
    expect(shouldAutoAcceptCodexTrustPrompt('OpenAI Codex v0.120.0')).toBe(false)
  })
})

describe('isCodexReadyForPromptInjection', () => {
  it('returns false on trust gate output', () => {
    const raw =
      '\u001b[3;3HDo\u001b[3;6Hyou\u001b[3;10Htrust\u001b[3;16Hthe\u001b[3;20Hcontents\u001b[3;29Hof\u001b[3;32Hthis\u001b[3;37Hdirectory?\u001b[9;3H\u001b[2mPress enter to continue\u001b[m'
    expect(isCodexReadyForPromptInjection(raw)).toBe(false)
  })

  it('returns true once OpenAI Codex home screen is visible', () => {
    const raw = '\u001b[2m│ \u001b[22m\u001b[1mOpenAI Codex\u001b[22m\u001b[2m (v0.120.0) │\u001b[m'
    expect(isCodexReadyForPromptInjection(raw)).toBe(true)
  })
})

describe('shouldAutoAcceptSessionEditPrompt', () => {
  it('returns true for claude-style allow all edits prompt', () => {
    const raw = [
      'Do you want to create libobs-metal__metal-subsystem.swift.md?',
      '1. Yes',
      '2. Yes, allow all edits during this session (shift+tab)',
      '3. No'
    ].join('\n')
    expect(shouldAutoAcceptSessionEditPrompt(raw)).toBe(true)
  })

  it('returns true for the per-tool "Do you want to proceed" prompt with a persistent option', () => {
    const raw = [
      'Bash command',
      '  mkdir -p /repo/.multi-ai-code/repo-view/analyses',
      '  Ensure analyses cache directory exists',
      '',
      'Do you want to proceed?',
      '❯ 1. Yes',
      '  2. Yes, and always allow access to analyses/ from this project',
      '  3. No'
    ].join('\n')
    expect(shouldAutoAcceptSessionEditPrompt(raw)).toBe(true)
  })

  it('returns false for a per-tool "Do you want to proceed" prompt without a persistent option', () => {
    const raw = [
      'Do you want to proceed?',
      '❯ 1. Yes',
      '  2. No'
    ].join('\n')
    expect(shouldAutoAcceptSessionEditPrompt(raw)).toBe(false)
  })

  it('returns false for unrelated output', () => {
    expect(shouldAutoAcceptSessionEditPrompt('OpenAI Codex v0.120.0')).toBe(false)
  })
})
