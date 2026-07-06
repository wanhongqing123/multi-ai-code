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

  it('drops Codex startup hints and model status while preserving idle greeting output', () => {
    const noisy = [
      'Use /skills to list available skills',
      '',
      'gpt-5.5 xhigh · ~/u4Quark/quarkpc/src · gpt-5.5 · src · Context 4% used · 5h 100% left · weekly 61% left',
      '',
      '你好，我在。有什么需要我处理的直接发我。',
      '',
      '## 处理结果',
      '',
      '- 已完成有效任务输出。'
    ].join('\n')

    expect(sanitizeRemoteImAicliOutput(noisy, { sourceKind: 'codex' })).toBe(
      ['你好，我在。有什么需要我处理的直接发我。', '', '## 处理结果', '', '- 已完成有效任务输出。'].join('\n')
    )
  })

  it('drops split Codex status blocks without dropping normal content lines', () => {
    const noisy = [
      'Use /skills to list available skills',
      '',
      'gpt-5.5 xhigh',
      '~/u4Quark/quarkpc/src',
      'gpt-5.5',
      'src',
      'Context 4% used',
      '5h 100% left',
      'weekly 61% left',
      '',
      '## 处理结果',
      '',
      'src',
      '- Codex 已处理有效输出。'
    ].join('\n')

    expect(sanitizeRemoteImAicliOutput(noisy, { sourceKind: 'codex' })).toBe(
      ['## 处理结果', '', 'src', '- Codex 已处理有效输出。'].join('\n')
    )
  })

  it('drops Codex composer suggestion lines from tagged IM output', () => {
    const noisy = [
      '› Run /review on my current changes',
      '',
      '我撤回了刚才所有本地源码改动，确认 git diff 为空后，用原分支源码重跑：',
      '',
      './sonic_build_cxx.sh config_mac.ini'
    ].join('\n')

    expect(sanitizeRemoteImAicliOutput(noisy, { sourceKind: 'codex' })).toBe(
      [
        '我撤回了刚才所有本地源码改动，确认 git diff 为空后，用原分支源码重跑：',
        '',
        './sonic_build_cxx.sh config_mac.ini'
      ].join('\n')
    )
  })

  it('drops Codex starter suggestions even when the prompt glyph is not captured', () => {
    const noisy = [
      'Write tests for @filename',
      'Find and fix a bug in @filename',
      '',
      '你好，我在。需要我帮你看代码、查日志、编译或处理 CR 都可以。'
    ].join('\n')

    expect(sanitizeRemoteImAicliOutput(noisy, { sourceKind: 'codex' })).toBe(
      '你好，我在。需要我帮你看代码、查日志、编译或处理 CR 都可以。'
    )
  })

  it('does not apply Codex-only noise filters to Claude output', () => {
    const output = [
      'Use /skills to list available skills',
      '你好，我在。有什么需要我处理的直接发我。'
    ].join('\n')

    expect(sanitizeRemoteImAicliOutput(output, { sourceKind: 'claude' })).toBe(output)
    expect(sanitizeRemoteImAicliOutput(output)).toBe(output)
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
