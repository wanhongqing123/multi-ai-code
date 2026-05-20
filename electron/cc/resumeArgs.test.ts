import { describe, expect, it } from 'vitest'
import { buildResumeArgs } from './resumeArgs.js'

describe('buildResumeArgs - claude', () => {
  it('prepends --continue to empty args', () => {
    expect(buildResumeArgs('claude', [])).toEqual(['--continue'])
  })

  it('preserves non-conflicting flags', () => {
    expect(buildResumeArgs('claude', ['--model', 'opus', '--effort', 'high'])).toEqual([
      '--continue',
      '--model',
      'opus',
      '--effort',
      'high'
    ])
  })

  it('drops an existing --continue / -c without duplicating', () => {
    expect(buildResumeArgs('claude', ['--continue', '--model', 'opus'])).toEqual([
      '--continue',
      '--model',
      'opus'
    ])
    expect(buildResumeArgs('claude', ['-c', '--model', 'opus'])).toEqual([
      '--continue',
      '--model',
      'opus'
    ])
  })

  it('drops --resume <id> and its value', () => {
    expect(buildResumeArgs('claude', ['--resume', 'abc-uuid', '--model', 'opus'])).toEqual([
      '--continue',
      '--model',
      'opus'
    ])
    expect(buildResumeArgs('claude', ['-r', 'abc-uuid'])).toEqual(['--continue'])
  })

  it('drops --resume= equals form', () => {
    expect(buildResumeArgs('claude', ['--resume=abc-uuid', '--model', 'opus'])).toEqual([
      '--continue',
      '--model',
      'opus'
    ])
  })

  it('does NOT consume a following flag as value when --resume is bare', () => {
    // If user passed `--resume --model opus`, the value-less --resume should
    // not eat `--model` as its value; we only drop the flag itself.
    expect(buildResumeArgs('claude', ['--resume', '--model', 'opus'])).toEqual([
      '--continue',
      '--model',
      'opus'
    ])
  })

  it('drops --fork-session', () => {
    expect(buildResumeArgs('claude', ['--fork-session', '--model', 'opus'])).toEqual([
      '--continue',
      '--model',
      'opus'
    ])
  })
})

describe('buildResumeArgs - codex', () => {
  it('prepends `resume --last` to empty args', () => {
    expect(buildResumeArgs('codex', [])).toEqual(['resume', '--last'])
  })

  it('preserves non-conflicting flags', () => {
    expect(buildResumeArgs('codex', ['-c', 'model="o3"'])).toEqual([
      'resume',
      '--last',
      '-c',
      'model="o3"'
    ])
  })

  it('drops an existing `resume` subcommand and its positional args', () => {
    expect(
      buildResumeArgs('codex', ['resume', 'some-uuid', 'leftover-prompt', '-c', 'k=v'])
    ).toEqual(['resume', '--last', '-c', 'k=v'])
  })

  it('drops a `fork` subcommand block', () => {
    expect(buildResumeArgs('codex', ['fork', 'uuid', '-c', 'k=v'])).toEqual([
      'resume',
      '--last',
      '-c',
      'k=v'
    ])
  })

  it('leaves non-subcommand-leading args alone', () => {
    expect(buildResumeArgs('codex', ['-c', 'model="o3"', 'extra'])).toEqual([
      'resume',
      '--last',
      '-c',
      'model="o3"',
      'extra'
    ])
  })
})
