export const JSON_REPLY_START = 'MAC_EXTERNAL_REVIEW_JSON_START'
export const JSON_REPLY_END = 'MAC_EXTERNAL_REVIEW_JSON_END'

export type ExternalReviewDecisionValue = 'accepted' | 'rejected' | 'needs-human'

export interface ExternalReviewAssessmentItem {
  title: string
  reason: string
  fileHint?: string
  lineHint?: string
  recommendation?: string
}

export interface ExternalReviewDecision {
  decision: ExternalReviewDecisionValue
  reason: string
  acceptedChanges?: ExternalReviewAssessmentItem[]
  rejectedChanges?: ExternalReviewAssessmentItem[]
  modificationPlan?: string[]
}

export interface ExternalReviewSuggestion {
  rawText: string
  pathHint: string | null
  lineHint: string | null
  linkedDiffFile: ExternalReviewDiffFile | null
}

export interface ExternalReviewDiffFile {
  path: string
}

export interface ExternalReviewPromptArgs {
  planAbsPath: string
  suggestion: ExternalReviewSuggestion
}

const VALID_DECISIONS = new Set<ExternalReviewDecisionValue>(['accepted', 'rejected', 'needs-human'])

export function buildExternalReviewPrompt({
  planAbsPath,
  suggestion
}: ExternalReviewPromptArgs): string {
  return [
    '认真查看下这个由其他外部AI的工具生成的Review建议，详细评价建议的合理性，哪些建议是合理的可以修改的，哪些是不合理的没必要修改的。给出详细的表格和修改方案。',
    '请直接输出 Markdown 结果，不要输出 JSON。',
    `Plan file: ${planAbsPath}`,
    '',
    'External review content (Markdown):',
    suggestion.rawText,
    '',
    '请严格按下面结构输出（中文，简明扼要）：',
    '## 结论',
    '- 一句话总评（建议采纳/不采纳/部分采纳）',
    '',
    '## 需修改（建议采纳）',
    '| 问题 | 位置 | 理由 | 修改建议 |',
    '| --- | --- | --- | --- |',
    '| ... | ... | ... | ... |',
    '',
    '## 无需修改（不建议采纳）',
    '| 问题 | 理由 |',
    '| --- | --- |',
    '| ... | ... |',
    '',
    '## 修改方案',
    '1. ...',
    '2. ...',
    '',
    '必须把完整 Markdown 放在以下标识之间：',
    `Start sentinel line: ${JSON_REPLY_START}`,
    `End sentinel line: ${JSON_REPLY_END}`,
    '标识外不要重复内容。',
  ].join('\n')
}

export function extractTaggedJsonReply(output: string): ExternalReviewDecision | null {
  const start = output.indexOf(JSON_REPLY_START)
  if (start === -1) return null

  const afterStart = output.slice(start + JSON_REPLY_START.length)
  const end = afterStart.indexOf(JSON_REPLY_END)
  if (end === -1) return null

  const taggedBlock = afterStart.slice(0, end).trim()
  const cleaned = stripTerminalControlSequences(taggedBlock).trim()
  if (!cleaned) return null

  const decision = inferDecisionFromMarkdown(cleaned)
  return {
    decision,
    reason: cleaned
  }
}

function stripTerminalControlSequences(value: string): string {
  return value
    .replace(/\u001b\][^\u0007]*(?:\u0007|\u001b\\)/g, '')
    .replace(/\u001b\[[0-?]*[ -/]*[@-~]/g, '')
    .replace(/\u001b[@-_]/g, '')
    .replace(/\[(?:\d{1,4};){0,8}\d{1,4}[A-Za-z]/g, '')
    .replace(/[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/g, '')
}

function inferDecisionFromMarkdown(text: string): ExternalReviewDecisionValue {
  const normalized = text.toLowerCase()
  const hasAdopt = normalized.includes('建议采纳') || normalized.includes('需修改（建议采纳）')
  const hasReject =
    normalized.includes('不采纳') ||
    normalized.includes('不建议采纳') ||
    normalized.includes('无需修改（不建议采纳）')

  if (hasReject && !hasAdopt) {
    return 'rejected'
  }
  if (hasAdopt && !hasReject) {
    return 'accepted'
  }
  return 'needs-human'
}
