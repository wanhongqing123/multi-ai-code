import { describe, expect, it } from 'vitest'
import {
  buildRepoUserMessageText,
  syncAssistantMessage,
  type RepoConversationMessage
} from './repoConversation.js'

describe('buildRepoUserMessageText', () => {
  it('summarizes file, annotation count, and optional question', () => {
    const text = buildRepoUserMessageText({
      filePath: 'src/app.ts',
      annotationCount: 2,
      question: '请解释并直接修复这里的问题'
    })

    expect(text).toContain('src/app.ts')
    expect(text).toContain('2 条标注')
    expect(text).toContain('请解释并直接修复这里的问题')
  })
})

describe('syncAssistantMessage', () => {
  it('updates the pending assistant message in place while streaming', () => {
    const before: RepoConversationMessage[] = [
      { id: 'u1', role: 'user', text: 'question' },
      { id: 'a1', role: 'assistant', text: 'partial', streaming: true }
    ]

    const next = syncAssistantMessage(before, 'complete answer', false)

    expect(next).toEqual([
      { id: 'u1', role: 'user', text: 'question' },
      { id: 'a1', role: 'assistant', text: 'complete answer', streaming: false }
    ])
  })

  it('appends a new assistant message when none exists yet', () => {
    const next = syncAssistantMessage(
      [{ id: 'u1', role: 'user', text: 'question' }],
      'first answer',
      true
    )

    expect(next).toHaveLength(2)
    expect(next[1]?.role).toBe('assistant')
    expect(next[1]?.text).toBe('first answer')
    expect(next[1]?.streaming).toBe(true)
  })
})
