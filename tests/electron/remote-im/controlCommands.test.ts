import { describe, expect, it } from 'vitest'
import { parseRemoteImControlCommand } from '../../../electron/remote-im/controlCommands.js'

describe('remote IM control commands', () => {
  it('parses supported commands only when the message starts with slash', () => {
    expect(parseRemoteImControlCommand('/status')).toEqual({
      type: 'command',
      command: 'status',
      raw: '/status',
      args: ''
    })
    expect(parseRemoteImControlCommand('  /plan  ')).toEqual({
      type: 'command',
      command: 'plan',
      raw: '/plan',
      args: ''
    })
    expect(parseRemoteImControlCommand('/build 请继续')).toEqual({
      type: 'command',
      command: 'build',
      raw: '/build 请继续',
      args: '请继续'
    })
    expect(parseRemoteImControlCommand('/model 2')).toEqual({
      type: 'command',
      command: 'model',
      raw: '/model 2',
      args: '2'
    })
    expect(parseRemoteImControlCommand('/models')).toEqual({
      type: 'command',
      command: 'models',
      raw: '/models',
      args: ''
    })
    expect(parseRemoteImControlCommand('/goal 修复 IM 回传')).toEqual({
      type: 'command',
      command: 'goal',
      raw: '/goal 修复 IM 回传',
      args: '修复 IM 回传'
    })
    expect(parseRemoteImControlCommand('/btw 检查构建日志')).toEqual({
      type: 'command',
      command: 'btw',
      raw: '/btw 检查构建日志',
      args: '检查构建日志'
    })
    expect(parseRemoteImControlCommand('/diff --stat electron')).toEqual({
      type: 'command',
      command: 'diff',
      raw: '/diff --stat electron',
      args: '--stat electron'
    })
    expect(parseRemoteImControlCommand('/interrupt')).toEqual({
      type: 'command',
      command: 'interrupt',
      raw: '/interrupt',
      args: ''
    })
    expect(parseRemoteImControlCommand('/compact')).toEqual({
      type: 'command',
      command: 'compact',
      raw: '/compact',
      args: ''
    })
    expect(parseRemoteImControlCommand('/clear')).toEqual({
      type: 'command',
      command: 'clear',
      raw: '/clear',
      args: ''
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

  it('lets slash-leading paths and non-word tokens through as normal text', () => {
    // 以路径开头的日常开发消息不能被当成敲错的命令拒收。
    expect(parseRemoteImControlCommand('/etc/hosts 这个文件怎么改')).toEqual({ type: 'text' })
    expect(parseRemoteImControlCommand('/tmp/app.log 看下这个日志')).toEqual({ type: 'text' })
    expect(parseRemoteImControlCommand('/v2 接口返回什么')).toEqual({ type: 'text' })
    expect(parseRemoteImControlCommand('/.env 里加个变量')).toEqual({ type: 'text' })
    // 纯字母单词仍按未知命令拒收提示。
    expect(parseRemoteImControlCommand('/stauts')).toEqual({
      type: 'unknown-command',
      commandText: '/stauts'
    })
  })

})
