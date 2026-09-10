import { describe, expect, it } from 'vitest'
import { getRemoteImAicliOutputSourceKind } from '../../../electron/remote-im/aicliSourceKind.js'

describe('remote IM AICLI source kind', () => {
  it('recognizes Claude and Codex commands', () => {
    expect(getRemoteImAicliOutputSourceKind('claude')).toBe('claude')
    expect(getRemoteImAicliOutputSourceKind('codex')).toBe('codex')
  })

  it('recognizes absolute paths and Windows command suffixes', () => {
    expect(getRemoteImAicliOutputSourceKind('/repo/bin/aicli/codex/darwin-arm64/codex')).toBe(
      'codex'
    )
    expect(getRemoteImAicliOutputSourceKind('C:\\Tools\\codex.exe')).toBe('codex')
    expect(getRemoteImAicliOutputSourceKind('"C:\\Tools\\claude.cmd"')).toBe('claude')
  })

  it('does not misclassify wrapper names', () => {
    expect(getRemoteImAicliOutputSourceKind('my-claude-wrapper')).toBe('unknown')
    expect(getRemoteImAicliOutputSourceKind('/tools/codex-wrapper')).toBe('unknown')
  })
})
