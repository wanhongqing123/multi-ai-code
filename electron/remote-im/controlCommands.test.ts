import { describe, expect, it } from 'vitest'
import {
  REMOTE_IM_CONTROL_COMMANDS,
  formatRemoteImControlCommandHelp,
  parseRemoteImControlCommand
} from './controlCommands.js'

describe('remote IM control commands', () => {
  it('parses supported commands only when the message starts with slash', () => {
    expect(parseRemoteImControlCommand('/status')).toEqual({
      type: 'command',
      command: 'status',
      raw: '/status'
    })
    expect(parseRemoteImControlCommand('  /plan  ')).toEqual({
      type: 'command',
      command: 'plan',
      raw: '/plan'
    })
    expect(parseRemoteImControlCommand('/build 请继续')).toEqual({
      type: 'command',
      command: 'build',
      raw: '/build 请继续'
    })
    expect(parseRemoteImControlCommand('请执行 /status')).toEqual({ type: 'text' })
  })

  it('keeps unknown slash input in the command channel', () => {
    expect(parseRemoteImControlCommand('/review')).toEqual({
      type: 'unknown-command',
      commandText: '/review'
    })
    expect(parseRemoteImControlCommand('/stop')).toEqual({
      type: 'unknown-command',
      commandText: '/stop'
    })
  })

  it('does not expose stop as a supported command', () => {
    expect(REMOTE_IM_CONTROL_COMMANDS.map((command) => command.name)).toEqual([
      'status',
      'plan',
      'build',
      'help'
    ])
    expect(formatRemoteImControlCommandHelp()).not.toContain('/stop')
  })
})
