import { describe, expect, it } from 'vitest'
import {
  createTerminalMarkdownState,
  formatMarkdownChunk,
  shouldFormatMarkdownForCli,
  stripAnsi
} from './terminalMarkdown.js'

describe('shouldFormatMarkdownForCli', () => {
  it('bypasses markdown formatting for opencode in any command form', () => {
    expect(shouldFormatMarkdownForCli('opencode')).toBe(false)
    expect(shouldFormatMarkdownForCli('OpenCode')).toBe(false)
    expect(shouldFormatMarkdownForCli('opencode.exe')).toBe(false)
    expect(shouldFormatMarkdownForCli('C:\\Tools\\opencode.exe')).toBe(false)
    expect(shouldFormatMarkdownForCli('/usr/local/bin/opencode')).toBe(false)
  })

  it('bypasses markdown formatting for codex (full-screen ratatui TUI) in any command form', () => {
    // codex 也是全屏 TUI，改写行宽/删行会让它的光标定位错位（任务执行时光标乱跳、
    // 无法打字），必须与 opencode 一样原样直通。
    expect(shouldFormatMarkdownForCli('codex')).toBe(false)
    expect(shouldFormatMarkdownForCli('Codex')).toBe(false)
    expect(shouldFormatMarkdownForCli('codex.exe')).toBe(false)
    expect(shouldFormatMarkdownForCli('C:\\Tools\\codex.exe')).toBe(false)
    expect(shouldFormatMarkdownForCli('/usr/local/bin/codex')).toBe(false)
  })

  it('keeps markdown formatting for claude and unknown CLIs', () => {
    expect(shouldFormatMarkdownForCli('claude')).toBe(true)
    expect(shouldFormatMarkdownForCli('unknown')).toBe(true)
    expect(shouldFormatMarkdownForCli(undefined)).toBe(true)
    expect(shouldFormatMarkdownForCli('my-opencode-wrapper')).toBe(true)
    expect(shouldFormatMarkdownForCli('my-codex-wrapper')).toBe(true)
  })
})

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

  it('keeps existing ANSI color while still formatting markdown headings', () => {
    const state = createTerminalMarkdownState()
    const out = formatMarkdownChunk('\x1b[35m## 彩色标题\x1b[0m\n', state).text
    expect(stripAnsi(out)).toBe('彩色标题\n')
    expect(out).toContain('\x1b[35m')
    expect(out).not.toContain('##')
  })

  it('keeps ANSI-colored text while formatting bullets and inline code', () => {
    const state = createTerminalMarkdownState()
    const out = formatMarkdownChunk(
      '\x1b[33m- 关注 `colored()` 调用路径\x1b[0m\n',
      state
    ).text
    expect(stripAnsi(out)).toBe('• 关注 colored() 调用路径\n')
    expect(out).toContain('\x1b[33m')
    expect(out).toContain('\x1b[36m')
  })

  it('passes through ansi cursor-control tui lines untouched', () => {
    const state = createTerminalMarkdownState()
    const line = '\x1b[2K\x1b[1G## 不要改这类 TUI 重绘行\x1b[0m\n'
    expect(formatMarkdownChunk(line, state).text).toBe(line)
  })

  it('passes through ansi background-highlight lines untouched', () => {
    const state = createTerminalMarkdownState()
    const line = '\x1b[48;5;252m## 带背景色的状态行      \x1b[0m\n'
    expect(formatMarkdownChunk(line, state).text).toBe(line)
  })
})
