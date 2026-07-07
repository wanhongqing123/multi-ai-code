import { describe, expect, it } from 'vitest'
import { normalizeTerminalStyleForCli } from './terminalCodexStyle'

describe('normalizeTerminalStyleForCli', () => {
  it('removes Codex truecolor background and reverse-video SGR only', () => {
    const input =
      '\r\x1b[?25l\x1b[2K\x1b[1G\x1b[48;2;41;41;41;38;2;246;226;183;7m> hello\x1b[0m\x1b[?25h'

    expect(normalizeTerminalStyleForCli(input, 'codex', { platform: 'Win32' })).toBe(
      '\r\x1b[?25l\x1b[2K\x1b[1G\x1b[38;2;246;226;183m> hello\x1b[0m\x1b[?25h'
    )
  })

  it('removes Codex indexed background while preserving foreground color', () => {
    const input = '\x1b[48;5;236;38;5;15mstatus\x1b[0m'

    expect(normalizeTerminalStyleForCli(input, 'codex', { platform: 'Win32' })).toBe(
      '\x1b[38;5;15mstatus\x1b[0m'
    )
  })

  it('preserves cursor movement and line clearing sequences', () => {
    const input = '\r\x1b[2K\x1b[1G\x1b[12;34H\x1b[K'

    expect(normalizeTerminalStyleForCli(input, 'codex', { platform: 'Win32' })).toBe(input)
  })

  it('leaves Codex output unchanged on macOS', () => {
    const input =
      '\r\x1b[2K\x1b[1G\x1b[48;2;41;41;41;38;2;246;226;183;7m> hello\x1b[0m'

    expect(normalizeTerminalStyleForCli(input, 'codex', { platform: 'MacIntel' })).toBe(input)
  })

  it('leaves Claude output unchanged', () => {
    const input =
      '\r\x1b[2K\x1b[1G\x1b[48;2;41;41;41;38;2;246;226;183;7m> hello\x1b[0m'

    expect(normalizeTerminalStyleForCli(input, 'claude', { platform: 'Win32' })).toBe(input)
  })
})
