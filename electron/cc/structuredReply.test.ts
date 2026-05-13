import { describe, expect, it } from 'vitest'
import {
  JSON_REPLY_END,
  JSON_REPLY_START,
  buildExternalReviewPrompt,
  extractTaggedJsonReply,
  type ExternalReviewPromptArgs
} from './structuredReply.js'

describe('buildExternalReviewPrompt', () => {
  it('requests markdown table analysis instead of JSON payload', () => {
    const args: ExternalReviewPromptArgs = {
      planAbsPath: 'C:/plans/review-plan.md',
      suggestion: {
        rawText: 'External review content',
        pathHint: null,
        lineHint: null,
        linkedDiffFile: null
      }
    }
    const prompt = buildExternalReviewPrompt(args)

    expect(prompt).toContain('认真查看下这个由其他外部AI的工具生成的Review建议')
    expect(prompt).toContain('请直接输出 Markdown 结果，不要输出 JSON。')
    expect(prompt).toContain('## 需修改（建议采纳）')
    expect(prompt).toContain('## 无需修改（不建议采纳）')
    expect(prompt).toContain('## 修改方案')
    expect(prompt).toContain(`Start sentinel line: ${JSON_REPLY_START}`)
    expect(prompt).toContain(`End sentinel line: ${JSON_REPLY_END}`)
    expect(prompt).not.toContain('"decision":"accepted|rejected|needs-human"')
  })
})

describe('extractTaggedJsonReply', () => {
  it('returns null until sentinel block is complete', () => {
    const partial = `noise\n${JSON_REPLY_START}\n## 结论\n- 建议采纳`
    expect(extractTaggedJsonReply(partial)).toBeNull()
  })

  it('extracts markdown payload and infers accepted', () => {
    const output = [
      'log',
      JSON_REPLY_START,
      '## 结论',
      '- 建议采纳',
      '## 需修改（建议采纳）',
      '| 问题 | 位置 | 理由 | 修改建议 |',
      '| --- | --- | --- | --- |',
      '| 空指针检查 | DefaultVideoPlayer.cpp:66 | 风险明确 | 增加null检查 |',
      JSON_REPLY_END
    ].join('\n')

    expect(extractTaggedJsonReply(output)).toEqual({
      decision: 'accepted',
      reason: [
        '## 结论',
        '- 建议采纳',
        '## 需修改（建议采纳）',
        '| 问题 | 位置 | 理由 | 修改建议 |',
        '| --- | --- | --- | --- |',
        '| 空指针检查 | DefaultVideoPlayer.cpp:66 | 风险明确 | 增加null检查 |'
      ].join('\n')
    })
  })

  it('returns a valid decision for markdown containing non-adoption verdict', () => {
    const output = [
      JSON_REPLY_START,
      '## 结论',
      '- 不建议采纳',
      '## 无需修改（不建议采纳）',
      '| 问题 | 理由 |',
      '| --- | --- |',
      '| 风格重命名 | 与当前修复无关 |',
      JSON_REPLY_END
    ].join('\n')

    expect(extractTaggedJsonReply(output)).toEqual({
      decision: 'needs-human',
      reason: [
        '## 结论',
        '- 不建议采纳',
        '## 无需修改（不建议采纳）',
        '| 问题 | 理由 |',
        '| --- | --- |',
        '| 风格重命名 | 与当前修复无关 |'
      ].join('\n')
    })
  })
})
