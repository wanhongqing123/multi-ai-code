import { describe, expect, it } from 'vitest'
import { scrollRuntimeLogToBottom } from './runtimeLogViewport.js'

describe('scrollRuntimeLogToBottom', () => {
  it('keeps the live runtime log viewport pinned to the newest output', () => {
    const element = {
      scrollTop: 0,
      scrollHeight: 2400
    }

    scrollRuntimeLogToBottom(element)

    expect(element.scrollTop).toBe(2400)
  })

  it('ignores a missing log element during the first render', () => {
    expect(() => scrollRuntimeLogToBottom(null)).not.toThrow()
  })
})
