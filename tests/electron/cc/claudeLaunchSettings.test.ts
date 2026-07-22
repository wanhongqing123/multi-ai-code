import { describe, expect, it } from 'vitest'
import {
  EMBEDDED_CLAUDE_SETTINGS,
  withEmbeddedClaudeSettings
} from '../../../electron/cc/claudeLaunchSettings.js'

describe('withEmbeddedClaudeSettings', () => {
  it('appends a default TUI override for embedded Claude sessions', () => {
    const args = withEmbeddedClaudeSettings('claude', ['--model', 'opus'])

    expect(args.slice(0, 2)).toEqual(['--model', 'opus'])
    expect(args[2]).toBe('--settings')
    expect(JSON.parse(args[3])).toEqual(EMBEDDED_CLAUDE_SETTINGS)
  })

  it('does not alter non-Claude commands', () => {
    expect(withEmbeddedClaudeSettings('codex', ['--ask-for-approval', 'never'])).toEqual([
      '--ask-for-approval',
      'never'
    ])
  })

  it('recognizes Windows Claude executable paths', () => {
    const args = withEmbeddedClaudeSettings('C:\\Users\\me\\.local\\bin\\claude.exe', [])

    expect(args).toEqual(['--settings', JSON.stringify(EMBEDDED_CLAUDE_SETTINGS)])
  })
})
