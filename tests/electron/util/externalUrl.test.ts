import { describe, expect, it } from 'vitest'
import { normalizeExternalHttpUrl } from '../../../electron/util/externalUrl.js'

describe('normalizeExternalHttpUrl', () => {
  it('accepts and normalizes HTTP and HTTPS URLs', () => {
    expect(normalizeExternalHttpUrl(' https://Example.com/path?q=1 ')).toBe(
      'https://example.com/path?q=1'
    )
    expect(normalizeExternalHttpUrl('http://example.com')).toBe('http://example.com/')
  })

  it('rejects non-web protocols and malformed values', () => {
    expect(normalizeExternalHttpUrl('javascript:alert(1)')).toBeNull()
    expect(normalizeExternalHttpUrl('file:///tmp/report.html')).toBeNull()
    expect(normalizeExternalHttpUrl('not a url')).toBeNull()
    expect(normalizeExternalHttpUrl('')).toBeNull()
  })
})
