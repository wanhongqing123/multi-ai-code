import { readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { describe, expect, it } from 'vitest'
import {
  REMOTE_IM_REPLY_CLOSE_TAG,
  REMOTE_IM_REPLY_OPEN_TAG,
  buildRemoteImAicliPrompt,
  buildRemoteImAicliDisplayText,
  extractRemoteImReplyOutput
} from './replyProtocol.js'

const __dirname = dirname(fileURLToPath(import.meta.url))

function readReplyFixture(name: string): string {
  return readFileSync(join(__dirname, 'fixtures', 'reply-protocol', name), 'utf8')
}

describe('remote IM reply protocol', () => {
  it('builds a compact AICLI prompt that avoids visible protocol echo fragments', () => {
    const prompt = buildRemoteImAicliPrompt({
      fromUserId: 'phone_admin',
      text: 'check build',
      replyId: 'reply-123'
    })

    expect(prompt).toContain('[来自远程 IM：phone_admin]')
    expect(prompt).toContain('check build')
    expect(prompt).toContain('<remote-im-reply id="reply-123">')
    expect(prompt).toContain('</remote-im-reply id="reply-123">')
    expect(prompt.split('\n')).not.toContain('<remote-im-reply id="reply-123">')
    expect(prompt.split('\n')).not.toContain('</remote-im-reply id="reply-123">')
    expect(prompt).toContain('如果需要查询或操作 IM，请先运行 imcli help')
    expect(prompt).toContain('如需把截图或本地图片发回 IM')
    expect(prompt).toContain('imcli send-image <user> <imagePath>')
    expect(prompt).toContain('imcli send-file <user> <filePath>')
    expect(prompt).not.toContain('Remote IM reply protocol:')
    expect(prompt).not.toContain(`${REMOTE_IM_REPLY_OPEN_TAG} and ${REMOTE_IM_REPLY_CLOSE_TAG}`)
    expect(prompt.split('\n')).toHaveLength(8)
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

  it('extracts only the reply content matching the expected reply id', () => {
    const output = [
      '<remote-im-reply id="old-reply">',
      'old result',
      '</remote-im-reply id="old-reply">',
      REMOTE_IM_REPLY_OPEN_TAG,
      'legacy result',
      REMOTE_IM_REPLY_CLOSE_TAG,
      '<remote-im-reply id="reply-123">',
      'current result',
      '</remote-im-reply id="reply-123">'
    ].join('\n')

    expect(extractRemoteImReplyOutput(output, { replyId: 'reply-123' })).toEqual({
      content: 'current result',
      pending: false,
      nextBuffer: ''
    })
  })

  it.each([
    {
      name: 'markers and reply body on the same line',
      output:
        '<remote-im-reply id="rim-0123456789abcdef">你好</remote-im-reply id="rim-0123456789abcdef">'
    },
    {
      name: 'model omitted marker quote and angle bracket',
      output: [
        '<remote-im-reply id="rim-0123456789abcdef你好',
        '</remote-im-reply id="rim-0123456789abcdef'
      ].join('\n')
    }
  ])('extracts Codex reply when $name', ({ output }) => {
    expect(extractRemoteImReplyOutput(output, { replyId: 'rim-0123456789abcdef' })).toEqual({
      content: '你好',
      pending: false,
      nextBuffer: ''
    })
  })

  it.each([
    {
      name: 'legacy markers without reply id',
      output: [REMOTE_IM_REPLY_OPEN_TAG, 'legacy reply', REMOTE_IM_REPLY_CLOSE_TAG].join('\n'),
      replyId: undefined,
      expected: 'legacy reply'
    },
    {
      name: 'matching id markers',
      output: [
        '<remote-im-reply id="rim-current">',
        'current id reply',
        '</remote-im-reply id="rim-current">'
      ].join('\n'),
      replyId: 'rim-current',
      expected: 'current id reply'
    },
    {
      name: 'matching id open marker with legacy close marker',
      output: [
        '<remote-im-reply id="rim-current">',
        'current id reply with legacy close',
        REMOTE_IM_REPLY_CLOSE_TAG
      ].join('\n'),
      replyId: 'rim-current',
      expected: 'current id reply with legacy close'
    }
  ])('extracts reply protocol variant: $name', ({ output, replyId, expected }) => {
    expect(extractRemoteImReplyOutput(output, { replyId })).toEqual({
      content: expected,
      pending: false,
      nextBuffer: ''
    })
  })

  it('does not close a current reply with a wrong reply id close tag', () => {
    const output = [
      '<remote-im-reply id="rim-current">',
      'must not forward yet',
      '</remote-im-reply id="rim-other">'
    ].join('\n')

    const reply = extractRemoteImReplyOutput(output, { replyId: 'rim-current' })

    expect(reply.content).toBe('')
    expect(reply.pending).toBe(true)
    expect(reply.nextBuffer).toContain('must not forward yet')
  })

  it('accepts a legacy close tag after a matching reply id open tag', () => {
    const output = [
      '<remote-im-reply id="rim-current">',
      'Claude reply with id open and legacy close',
      REMOTE_IM_REPLY_CLOSE_TAG
    ].join('\n')

    expect(extractRemoteImReplyOutput(output, { replyId: 'rim-current' })).toEqual({
      content: 'Claude reply with id open and legacy close',
      pending: false,
      nextBuffer: ''
    })
  })

  it('replays the Claude id-open legacy-close incident fixture', () => {
    expect(
      extractRemoteImReplyOutput(readReplyFixture('claude-id-open-legacy-close.txt'), {
        replyId: 'rim-current'
      })
    ).toEqual({
      content: 'Claude transcript reply with an id-bearing opening marker and a legacy closing marker.',
      pending: false,
      nextBuffer: ''
    })
  })

  it('does not treat echoed prompt marker instructions as a reply', () => {
    const promptEcho = buildRemoteImAicliPrompt({
      fromUserId: 'phone_admin',
      text: '检查构建',
      replyId: 'reply-123'
    })
    const output = [
      promptEcho,
      'Find and fix a bug in @filename',
      'Write tests for @filename'
    ].join('\n')

    expect(extractRemoteImReplyOutput(output, { replyId: 'reply-123' })).toEqual({
      content: '',
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
