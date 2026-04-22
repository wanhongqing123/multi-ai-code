import { describe, expect, it } from 'vitest'
import {
  createTerminalMarkdownState,
  formatMarkdownChunk,
  stripAnsi
} from './terminalMarkdown.js'

describe('formatMarkdownChunk', () => {
  it('formats headings without showing markdown markers', () => {
    const state = createTerminalMarkdownState()
    const out = formatMarkdownChunk('## 未来扩展\n', state).text
    expect(stripAnsi(out)).toBe('未来扩展\n')
    expect(out).toContain('\x1b[')
  })

  it('formats bullets, bold labels, inline code, and links', () => {
    const state = createTerminalMarkdownState()
    const out = formatMarkdownChunk(
      '- **HDR 工作流**: `VK_EXT_swapchain_colorspace` [spec](https://example.com)\n',
      state
    ).text
    expect(stripAnsi(out)).toBe(
      '• HDR 工作流: VK_EXT_swapchain_colorspace spec (https://example.com)\n'
    )
  })

  it('flattens markdown table rows and drops divider rows', () => {
    const state = createTerminalMarkdownState()
    const out = formatMarkdownChunk(
      '| 风险 | 缓解 |\n| --- | --- |\n| glslang 首次编译慢 | 走磁盘 SPIR-V 缓存 |\n',
      state
    ).text
    expect(stripAnsi(out)).toBe(
      '风险 │ 缓解\nglslang 首次编译慢 │ 走磁盘 SPIR-V 缓存\n'
    )
  })

  it('keeps fenced code blocks readable without inline markdown styling', () => {
    const state = createTerminalMarkdownState()
    const out = formatMarkdownChunk('```ts\nconst value = `raw`\n```\n', state).text
    expect(stripAnsi(out)).toBe('┌─ ts\nconst value = `raw`\n└─\n')
  })

  it('passes partial lines through untouched until a newline arrives', () => {
    const state = createTerminalMarkdownState()
    expect(formatMarkdownChunk('## 未完成标题', state).text).toBe('## 未完成标题')
    expect(formatMarkdownChunk('\n', state).text).toBe('\n')
  })
})
