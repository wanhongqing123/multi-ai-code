import { describe, expect, it } from 'vitest'
import { getRemoteImAicliOutputSourceKind } from './aicliSourceKind.js'

describe('remote IM AICLI source kind', () => {
  it('recognizes Claude, Codex, and OpenCode commands', () => {
    expect(getRemoteImAicliOutputSourceKind('claude')).toBe('claude')
    expect(getRemoteImAicliOutputSourceKind('codex')).toBe('codex')
    expect(getRemoteImAicliOutputSourceKind('opencode')).toBe('opencode')
  })

  it('recognizes absolute paths and Windows command suffixes', () => {
    expect(getRemoteImAicliOutputSourceKind('/repo/bin/aicli/opencode/darwin-arm64/opencode')).toBe(
      'opencode'
    )
    expect(getRemoteImAicliOutputSourceKind('C:\\Tools\\codex.exe')).toBe('codex')
    expect(getRemoteImAicliOutputSourceKind('"C:\\Tools\\opencode.cmd"')).toBe('opencode')
  })

  it('does not misclassify wrapper names', () => {
    expect(getRemoteImAicliOutputSourceKind('my-opencode-wrapper')).toBe('unknown')
    expect(getRemoteImAicliOutputSourceKind('/tools/codex-wrapper')).toBe('unknown')
  })
})
