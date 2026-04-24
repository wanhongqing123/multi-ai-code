import { describe, expect, it } from 'vitest'
import {
  shouldWaitForRepoCliReady,
  shouldAutoRespondToRepoPermissionPrompt
} from './repoAnalysisManager'

describe('shouldWaitForRepoCliReady', () => {
  it('does not wait when claude prompt is already marked ready', () => {
    expect(
      shouldWaitForRepoCliReady({
        command: 'claude',
        startedAt: 0,
        hasSeenOutput: false,
        claudePromptReady: true
      }, 1000)
    ).toBe(false)
  })

  it('does not wait when codex prompt is already marked ready', () => {
    expect(
      shouldWaitForRepoCliReady({
        command: 'codex',
        startedAt: 0,
        hasSeenOutput: false,
        codexPromptReady: true
      }, 1000)
    ).toBe(false)
  })

  it('does not wait once the session has shown output long enough to assume interactivity', () => {
    expect(
      shouldWaitForRepoCliReady({
        command: 'claude',
        startedAt: 0,
        hasSeenOutput: true
      }, 1500)
    ).toBe(false)
  })

  it('still waits when the session is fresh and no ready signal has been seen', () => {
    expect(
      shouldWaitForRepoCliReady({
        command: 'claude',
        startedAt: 0,
        hasSeenOutput: false
      }, 500)
    ).toBe(true)
  })
})

describe('shouldAutoRespondToRepoPermissionPrompt', () => {
  const prompt =
    'Do you want to create x.md?\n1. Yes\n2. Yes, allow all edits during this session (shift+tab)\n3. No'
  const analysesPrompt = [
    'Bash command',
    '  mkdir -p /repo/.multi-ai-code/repo-view/analyses',
    '  Ensure analyses cache directory exists',
    '',
    'Do you want to proceed?',
    '❯ 1. Yes',
    '  2. Yes, and always allow access to analyses/ from this project',
    '  3. No'
  ].join('\n')
  const unrelatedPersistentPrompt = [
    'Bash command',
    '  touch docs/notes.md',
    '',
    'Do you want to proceed?',
    '❯ 1. Yes',
    '  2. Yes, and always allow access to docs/ from this project',
    '  3. No'
  ].join('\n')

  it('responds the first time the prompt appears', () => {
    expect(
      shouldAutoRespondToRepoPermissionPrompt(
        { permissionPromptActive: false },
        prompt,
        1000
      )
    ).toEqual({ shouldRespond: true, promptActive: true, lastRespondAt: 1000 })
  })

  it('does not respond again while the same prompt remains visible in scrollback', () => {
    expect(
      shouldAutoRespondToRepoPermissionPrompt(
        {
          permissionPromptActive: true,
          lastPermissionRespondAt: 1000
        },
        prompt,
        5000
      )
    ).toEqual({ shouldRespond: false, promptActive: true, lastRespondAt: 1000 })
  })

  it('re-arms once the prompt disappears and later reappears', () => {
    const cleared = shouldAutoRespondToRepoPermissionPrompt(
      {
        permissionPromptActive: true,
        lastPermissionRespondAt: 1000
      },
      'normal output',
      2000
    )
    expect(cleared).toEqual({
      shouldRespond: false,
      promptActive: false,
      lastRespondAt: 1000
    })
    expect(
      shouldAutoRespondToRepoPermissionPrompt(
        {
          permissionPromptActive: cleared.promptActive,
          lastPermissionRespondAt: cleared.lastRespondAt
        },
        prompt,
        4000
      )
    ).toEqual({ shouldRespond: true, promptActive: true, lastRespondAt: 4000 })
  })

  it('responds to the repo-view analyses cache prompt', () => {
    expect(
      shouldAutoRespondToRepoPermissionPrompt(
        { permissionPromptActive: false },
        analysesPrompt,
        1000
      )
    ).toEqual({ shouldRespond: true, promptActive: true, lastRespondAt: 1000 })
  })

  it('does not auto-respond to unrelated persistent per-tool prompts', () => {
    expect(
      shouldAutoRespondToRepoPermissionPrompt(
        { permissionPromptActive: false },
        unrelatedPersistentPrompt,
        1000
      )
    ).toEqual({
      shouldRespond: false,
      promptActive: false,
      lastRespondAt: undefined
    })
  })
})
