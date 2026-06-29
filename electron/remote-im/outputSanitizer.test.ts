import { describe, expect, it } from 'vitest'
import { sanitizeRemoteImAicliOutput } from './outputSanitizer.js'

describe('remote IM output sanitizer', () => {
  it('drops Codex thinking/status terminal noise while preserving final markdown', () => {
    const noisy = [
      'thinking with xhigh effort)thinking with xhigh effort',
      '*thinking with xhigh effort✶✻thinking with xhigh effort✽',
      '354 cache 31.7K total in 32.3K/ out 354|ctx 3%/1.0M|5h 4%|7d 22%',
      '~\\AppData\\Local\\Temp\\multi-ai-code-mutual-vCHrNB\\repo-a',
      'Newspapering…✻Cogitated for 28s❯',
      '',
      '## Result',
      '',
      '- The operation completed.',
      '- `src/App.tsx` was updated.'
    ].join('\n')

    expect(sanitizeRemoteImAicliOutput(noisy)).toBe(
      ['## Result', '', '- The operation completed.', '- `src/App.tsx` was updated.'].join('\n')
    )
  })

  it('returns an empty string for redraw-only terminal UI chunks', () => {
    const redraw = [
      '────────────────────────────────────────────────────────',
      '❯ Press up to edit queued messages · ← for agents',
      'ctrl+g to edit in Notepad',
      'Cogitated for 28s'
    ].join('\n')

    expect(sanitizeRemoteImAicliOutput(redraw)).toBe('')
  })

  it('drops split Claude status redraws while preserving reply markdown', () => {
    const noisy = [
      '我是 Claude Code，Anthropic 出品的命令行编程助手。',
      '',
      'Opus |',
      '5h:19%',
      '7d:28% |',
      'ctx:3%/1M |',
      'cache:r15.8k+w2.6k |',
      'in:8.7k',
      'out:2',
      '● high · /effort',
      '←',
      'for',
      'agents',
      '',
      '- 熟悉多语言代码库的查阅、修改与重构'
    ].join('\n')

    expect(sanitizeRemoteImAicliOutput(noisy)).toBe(
      [
        '我是 Claude Code，Anthropic 出品的命令行编程助手。',
        '',
        '- 熟悉多语言代码库的查阅、修改与重构'
      ].join('\n')
    )
  })

  it('strips inline terminal redraws and remote IM prompt echo from Claude output', () => {
    const noisy =
      '·4thinking with xhigh effort6thinking with xhigh effort ❯ [来自远程 IM：multi_ai_code_e2e_a] 你好 ●你好！我是ClaudeCode，很高兴和你交流。\n' +
      '\n' +
      '- 阅读、分析或修改代码\n' +
      '- 调试问题、运行测试'

    expect(sanitizeRemoteImAicliOutput(noisy)).toBe(
      [
        '你好！我是ClaudeCode，很高兴和你交流。',
        '',
        '- 阅读、分析或修改代码',
        '- 调试问题、运行测试'
      ].join('\n')
    )
  })

})
