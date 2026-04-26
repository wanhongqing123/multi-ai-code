import { describe, expect, it } from 'vitest'
import {
  canSendRepoAnnotations,
  dispatchRepoSendQuestion,
  repoSendButtonTitle
} from './analysisPanelState'

describe('canSendRepoAnnotations', () => {
  it('returns false when session is not running', () => {
    expect(canSendRepoAnnotations(false, 2)).toBe(false)
  })

  it('returns false when there are no annotations', () => {
    expect(canSendRepoAnnotations(true, 0)).toBe(false)
  })

  it('returns true only when session is running and annotations exist', () => {
    expect(canSendRepoAnnotations(true, 1)).toBe(true)
  })

  it('returns false while a send is already in progress', () => {
    expect(canSendRepoAnnotations(true, 2, true)).toBe(false)
  })
})

describe('repoSendButtonTitle', () => {
  it('asks user to start cli first when session is stopped', () => {
    expect(repoSendButtonTitle(false, 3)).toBe('请先启动下方 AI CLI')
  })

  it('asks for at least one annotation when session is running but empty', () => {
    expect(repoSendButtonTitle(true, 0)).toBe('至少需要一条标注')
  })

  it('uses the short send label when sending is allowed', () => {
    expect(repoSendButtonTitle(true, 2)).toBe('发送')
  })

  it('shows a sending hint while the request is in flight', () => {
    expect(repoSendButtonTitle(true, 2, true)).toBe('发送中...')
  })
})

describe('dispatchRepoSendQuestion', () => {
  it('returns false when the send fails so the input is preserved', async () => {
    const sentQuestions: string[] = []

    const shouldClear = await dispatchRepoSendQuestion(
      '  investigate this branch  ',
      async (question) => {
        sentQuestions.push(question)
        return false
      }
    )

    expect(shouldClear).toBe(false)
    expect(sentQuestions).toEqual(['investigate this branch'])
  })

  it('returns true when the send succeeds so the input can clear', async () => {
    const sentQuestions: string[] = []

    const shouldClear = await dispatchRepoSendQuestion(
      '  summarize the risk  ',
      async (question) => {
        sentQuestions.push(question)
        return true
      }
    )

    expect(shouldClear).toBe(true)
    expect(sentQuestions).toEqual(['summarize the risk'])
  })

  it('passes the trimmed question to the send function', async () => {
    let receivedQuestion = ''

    await dispatchRepoSendQuestion('  explain this change  ', async (question) => {
      receivedQuestion = question
      return true
    })

    expect(receivedQuestion).toBe('explain this change')
  })
})
