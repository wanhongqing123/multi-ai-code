export const JSON_REPLY_START = 'MAC_EXTERNAL_REVIEW_JSON_START'
export const JSON_REPLY_END = 'MAC_EXTERNAL_REVIEW_JSON_END'

export type ExternalReviewDecisionValue = 'accepted' | 'rejected' | 'needs-human'

export interface ExternalReviewDecision {
  decision: ExternalReviewDecisionValue
  reason: string
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

const VALID_DECISIONS = new Set<ExternalReviewDecisionValue>([
  'accepted',
  'rejected',
  'needs-human'
])

export function buildExternalReviewPrompt({
  planAbsPath,
  suggestion
}: ExternalReviewPromptArgs): string {
  const linkedDiffPath = suggestion.linkedDiffFile?.path ?? '(none)'

  return [
    'Review the external review suggestion against the current plan and terminal context.',
    `Plan file: ${planAbsPath}`,
    '',
    'Suggestion:',
    suggestion.rawText,
    '',
    `Path hint: ${suggestion.pathHint ?? '(none)'}`,
    `Line hint: ${suggestion.lineHint ?? '(none)'}`,
    `Linked diff file: ${linkedDiffPath}`,
    '',
    'Respond with exactly one decision.',
    'Return a single JSON object wrapped by the stable sentinels listed below.',
    `Start sentinel line: ${JSON_REPLY_START}`,
    `End sentinel line: ${JSON_REPLY_END}`,
    'Do not include markdown fences or extra commentary.',
    'Only place your final decision JSON between those sentinel lines.',
    '{"decision":"accepted|rejected|needs-human","reason":"..."}',
  ].join('\n')
}

export function extractTaggedJsonReply(output: string): ExternalReviewDecision | null {
  const start = output.indexOf(JSON_REPLY_START)
  if (start === -1) return null

  const afterStart = output.slice(start + JSON_REPLY_START.length)
  const end = afterStart.indexOf(JSON_REPLY_END)
  if (end === -1) return null

  const jsonText = afterStart.slice(0, end).trim()
  const parsed = JSON.parse(jsonText) as unknown
  return validateExternalReviewDecision(parsed)
}

function validateExternalReviewDecision(value: unknown): ExternalReviewDecision {
  if (!value || typeof value !== 'object') {
    throw new Error('Tagged external review reply must be a JSON object.')
  }

  const candidate = value as Record<string, unknown>
  if (typeof candidate.decision !== 'string' || !VALID_DECISIONS.has(candidate.decision as ExternalReviewDecisionValue)) {
    throw new Error('Tagged external review reply has an invalid decision.')
  }

  if (typeof candidate.reason !== 'string' || candidate.reason.trim().length === 0) {
    throw new Error('Tagged external review reply must include a non-empty reason.')
  }

  return {
    decision: candidate.decision as ExternalReviewDecisionValue,
    reason: candidate.reason.trim()
  }
}
