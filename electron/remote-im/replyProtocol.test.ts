import { describe, expect, it } from 'vitest'
import {
  REMOTE_IM_REPLY_CLOSE_TAG,
  REMOTE_IM_REPLY_OPEN_TAG,
  buildRemoteImAicliPrompt,
  buildRemoteImAicliDisplayText,
  extractRemoteImReplyOutput
} from './replyProtocol.js'

describe('remote IM reply protocol', () => {
  it('builds a compact AICLI prompt that avoids visible protocol echo fragments', () => {
    const prompt = buildRemoteImAicliPrompt({
      fromUserId: 'phone_admin',
      text: 'check build'
    })

    expect(prompt).toContain('[来自远程 IM：phone_admin]')
    expect(prompt).toContain('check build')
    expect(prompt).toContain(REMOTE_IM_REPLY_OPEN_TAG)
    expect(prompt).toContain(REMOTE_IM_REPLY_CLOSE_TAG)
    expect(prompt).not.toContain('Remote IM reply protocol:')
    expect(prompt).not.toContain(`${REMOTE_IM_REPLY_OPEN_TAG} and ${REMOTE_IM_REPLY_CLOSE_TAG}`)
    expect(prompt.split('\n')).toHaveLength(7)
  })

  it('builds a terminal display summary without reply protocol instructions', () => {
    const displayText = buildRemoteImAicliDisplayText({
      fromUserId: 'phone_admin',
      text: 'check build'
    })

    expect(displayText).toBe('[来自远程 IM：phone_admin]\ncheck build')
    expect(displayText).not.toContain('[IM_REPLY]')
    expect(displayText).not.toContain(REMOTE_IM_REPLY_OPEN_TAG)
    expect(displayText).not.toContain(REMOTE_IM_REPLY_CLOSE_TAG)
  })

  it('extracts only completed tagged reply content', () => {
    const output = [
      'terminal noise',
      REMOTE_IM_REPLY_OPEN_TAG,
      '## Done',
      '',
      '- build passed',
      REMOTE_IM_REPLY_CLOSE_TAG,
      'more terminal noise'
    ].join('\n')

    expect(extractRemoteImReplyOutput(output)).toEqual({
      content: '## Done\n\n- build passed',
      pending: false,
      nextBuffer: ''
    })
  })

  it('drops untagged output instead of forwarding terminal UI noise', () => {
    expect(extractRemoteImReplyOutput('Assistant text.│AddedCLAUDE_CODE_DISABLE_MOUSE_CLICKS')).toEqual({
      content: '',
      pending: false,
      nextBuffer: ''
    })
  })

  it('keeps an incomplete tagged reply buffered until the close tag arrives', () => {
    expect(extractRemoteImReplyOutput(`noise\n${REMOTE_IM_REPLY_OPEN_TAG}\npartial`)).toEqual({
      content: '',
      pending: true,
      nextBuffer: `${REMOTE_IM_REPLY_OPEN_TAG}\npartial`
    })
  })

  it('extracts replies from Claude fullscreen terminal redraw chunks', () => {
    const fullscreenChunk = [
      `[IM_REPLY] Put final Markdown for IM between full-line ${REMOTE_IM_REPLY_OPEN_TAG} and ${REMOTE_IM_REPLY_CLOSE_TAG}; text outside tags is ignored.\r\n`,
      '\u001b[?25l\u001b[20;1H●\u001b[m\u001b[1C',
      `${REMOTE_IM_REPLY_OPEN_TAG}\u001b[1C\u001b[K\r\n`,
      '  debug-ok\u001b[K\r\n',
      '\u001b[23;1H  second line\u001b[K\r\n',
      `\u001b[26;3H${REMOTE_IM_REPLY_CLOSE_TAG}\u001b[K`,
      '\u001b[27;3H\u001b[K\r\n✻ Brewed for 5s'
    ].join('')

    expect(extractRemoteImReplyOutput(fullscreenChunk)).toEqual({
      content: 'debug-ok\nsecond line',
      pending: false,
      nextBuffer: ''
    })
  })

  it('ignores wrapped prompt echo tags before the assistant reply', () => {
    const fullscreenChunk = [
      '[IM_REPLY] Put final Markdown for IM between full-line ',
      `${REMOTE_IM_REPLY_OPEN_TAG}\r\n`,
      `and ${REMOTE_IM_REPLY_CLOSE_TAG}; text outside tags is ignored.\r\n`,
      '\u001b[20;1HOpus | 5h:19% 7d:28% | ctx:3%/1M | cache:r15.8k+w2.6k | in:8.7k out:2\u001b[K',
      '\u001b[10;1H⏺\r\n',
      `${REMOTE_IM_REPLY_OPEN_TAG}\r\n`,
      '  我是 Claude Code，能帮你处理工程任务。\r\n',
      `  ${REMOTE_IM_REPLY_CLOSE_TAG}\r\n`
    ].join('')

    expect(extractRemoteImReplyOutput(fullscreenChunk)).toEqual({
      content: '我是 Claude Code，能帮你处理工程任务。',
      pending: false,
      nextBuffer: ''
    })
  })

  it('keeps terminal column-positioned text on the same visual line', () => {
    const fullscreenChunk = [
      `${REMOTE_IM_REPLY_OPEN_TAG}\r\n`,
      '\u001b[2G我是\u001b[6GClaude\u001b[13GCode，Anthropic\u001b[29G出品\r\n',
      '\u001b[2Ghigh\u001b[7G·\u001b[9G/effort\r\n',
      `${REMOTE_IM_REPLY_CLOSE_TAG}\r\n`
    ].join('')

    expect(extractRemoteImReplyOutput(fullscreenChunk)).toEqual({
      content: ['我是 Claude Code，Anthropic 出品', 'high · /effort'].join('\n'),
      pending: false,
      nextBuffer: ''
    })
  })
})
