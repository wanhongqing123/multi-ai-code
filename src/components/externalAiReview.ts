export interface ExternalReviewDiffFile {
  path: string
}

export interface ExternalReviewSuggestion {
  id: string
  sourceLabel: string
  rawText: string
  pathHint: string | null
  lineHint: string | null
  linkedDiffFile: ExternalReviewDiffFile | null
  status: 'idle' | 'accepted' | 'rejected' | 'needs-human' | 'error'
  decisionReason: string
  decisionPayload?: ExternalReviewDecisionPayload | null
}

export interface ExternalReviewAssessmentItem {
  title: string
  reason: string
  fileHint?: string
  lineHint?: string
  recommendation?: string
}

export interface ExternalReviewDecisionPayload {
  decision: 'accepted' | 'rejected' | 'needs-human'
  reason: string
  acceptedChanges?: ExternalReviewAssessmentItem[]
  rejectedChanges?: ExternalReviewAssessmentItem[]
  modificationPlan?: string[]
}

const DECISION_JSON_START = 'MAC_EXTERNAL_REVIEW_JSON_START'
const DECISION_JSON_END = 'MAC_EXTERNAL_REVIEW_JSON_END'

const PATH_HINT_RE = /(?:\.{1,2}[\\/])?[A-Za-z0-9_.-]+(?:[\\/][A-Za-z0-9_.-]+)+\.[A-Za-z0-9]+/g

function normalizeText(text: string): string {
  return text.replace(/\r\n?/g, '\n')
}

function extractPathHint(text: string): string | null {
  const matches = text.match(PATH_HINT_RE)
  return matches?.[0] ?? null
}

function extractLineHint(text: string): string | null {
  const rangeMatch = text.match(/\b(?:L|line\s+)?(\d+)\s*-\s*(\d+)\b/i)
  if (rangeMatch) {
    return `${rangeMatch[1]}-${rangeMatch[2]}`
  }

  const singleMatch = text.match(/\b(?:L|line\s+)(\d+)\b/i)
  if (singleMatch) {
    return singleMatch[1]
  }

  return null
}

function slugify(value: string): string {
  const slug = value
    .trim()
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, '-')
    .replace(/^-+|-+$/g, '')

  return slug || 'source'
}

function hashSuggestionSeed(value: string): string {
  let hash = 2166136261

  for (let index = 0; index < value.length; index += 1) {
    hash ^= value.charCodeAt(index)
    hash = Math.imul(hash, 16777619)
  }

  return (hash >>> 0).toString(36)
}

function buildSuggestionId(sourceLabel: string, rawText: string, index: number): string {
  return `external-review-${slugify(sourceLabel)}-${index + 1}-${hashSuggestionSeed(`${sourceLabel}\n${rawText}`)}`
}

function buildSuggestion(sourceLabel: string, rawText: string, index: number): ExternalReviewSuggestion {
  return {
    id: buildSuggestionId(sourceLabel, rawText, index),
    sourceLabel,
    rawText,
    pathHint: extractPathHint(rawText),
    lineHint: extractLineHint(rawText),
    linkedDiffFile: null,
    status: 'idle',
    decisionReason: '',
    decisionPayload: null
  }
}

function normalizePathForMatch(path: string): string {
  return path.replace(/\\/g, '/').trim()
}

function extractJsonObject(text: string): string | null {
  const start = text.indexOf('{')
  const end = text.lastIndexOf('}')
  if (start < 0 || end < start) return null
  return text.slice(start, end + 1)
}

function toSingleLine(text: string): string {
  return text.replace(/\s*\n+\s*/g, ' ').replace(/\s{2,}/g, ' ').trim()
}

function truncate(text: string, maxChars = 220): string {
  if (text.length <= maxChars) return text
  return `${text.slice(0, maxChars - 3).trimEnd()}...`
}

function normalizeLine(value: string): string {
  return toSingleLine(value)
}

function formatAssessmentItems(
  title: string,
  items: ExternalReviewAssessmentItem[] | undefined
): string[] {
  if (!items || items.length === 0) return []
  const lines = [title]
  for (const item of items) {
    const heading = normalizeLine(item.title || '未命名建议')
    const reason = normalizeLine(item.reason || '')
    const location = [item.fileHint?.trim(), item.lineHint?.trim()].filter(Boolean).join(':')
    lines.push(`- ${heading}${location ? `（${location}）` : ''}${reason ? `：${reason}` : ''}`)
    if (item.recommendation?.trim()) {
      lines.push(`  建议：${normalizeLine(item.recommendation)}`)
    }
  }
  return lines
}

export function formatDecisionForDisplay(decision: ExternalReviewDecisionPayload): string {
  const hasStructuredSections =
    (decision.acceptedChanges?.length ?? 0) > 0 ||
    (decision.rejectedChanges?.length ?? 0) > 0 ||
    (decision.modificationPlan?.length ?? 0) > 0

  if (!hasStructuredSections) {
    const raw = formatDecisionReasonForDisplay(decision.reason ?? '')
    return raw || 'AI 已返回评审结果。'
  }

  const decisionLabel =
    decision.decision === 'accepted'
      ? '采纳'
      : decision.decision === 'rejected'
        ? '不采纳'
        : '需人工确认'

  const lines = [`结论：${decisionLabel}`]
  const summary = formatDecisionReasonForDisplay(decision.reason ?? '')
  if (summary) {
    lines.push(`总体说明：${summary}`)
  }

  const acceptedLines = formatAssessmentItems('需修改（建议采纳）：', decision.acceptedChanges)
  if (acceptedLines.length > 0) {
    lines.push('', ...acceptedLines)
  }

  const rejectedLines = formatAssessmentItems('无需修改（不建议采纳）：', decision.rejectedChanges)
  if (rejectedLines.length > 0) {
    lines.push('', ...rejectedLines)
  }

  if (decision.modificationPlan && decision.modificationPlan.length > 0) {
    lines.push('', '建议修改方案：')
    for (const step of decision.modificationPlan) {
      lines.push(`- ${normalizeLine(step)}`)
    }
  }

  return lines.join('\n').trim()
}

export function formatDecisionReasonForDisplay(rawReason: string): string {
  const normalized = normalizeText(rawReason).trim()
  if (!normalized) return ''

  const taggedPattern = new RegExp(
    `${DECISION_JSON_START}([\\s\\S]*?)${DECISION_JSON_END}`
  )
  const taggedMatch = normalized.match(taggedPattern)
  const taggedBody = taggedMatch?.[1]?.trim()
  const jsonCandidate = extractJsonObject(taggedBody ?? normalized)

  if (jsonCandidate) {
    try {
      const parsed = JSON.parse(jsonCandidate) as { reason?: unknown }
      if (typeof parsed.reason === 'string' && parsed.reason.trim().length > 0) {
        return truncate(toSingleLine(parsed.reason))
      }
    } catch {
      // Ignore parse error and fall through to plain-text sanitization.
    }
  }

  const cleaned = normalized
    .replaceAll(DECISION_JSON_START, '')
    .replaceAll(DECISION_JSON_END, '')
    .replace(/^\s*Path hint:.*$/gim, '')
    .replace(/^\s*Line hint:.*$/gim, '')
    .replace(/^\s*Linked diff file:.*$/gim, '')
    .replace(/^\s*Respond with exactly one decision\.\s*$/gim, '')
    .replace(/^\s*Return a single JSON object wrapped by.*$/gim, '')
    .replace(/^\s*Start sentinel line:.*$/gim, '')
    .replace(/^\s*End sentinel line:.*$/gim, '')
    .replace(/^\s*Do not include markdown fences.*$/gim, '')
    .replace(/^\s*Only place your final decision JSON.*$/gim, '')
    .trim()

  return truncate(toSingleLine(cleaned))
}

export function formatDecisionErrorForDisplay(rawError: string): string {
  const normalized = normalizeText(rawError).trim()
  if (!normalized) return 'AI 判断失败，请重试。'
  if (
    normalized.includes(DECISION_JSON_START) ||
    normalized.includes(DECISION_JSON_END)
  ) {
    return 'AI 返回格式不规范，已忽略原始协议内容。请重试。'
  }
  const lower = normalized.toLowerCase()

  if (lower.includes('timed out')) {
    return 'AI 判断超时，请重试。'
  }
  if (
    lower.includes('unexpected token') ||
    lower.includes('json') ||
    lower.includes('tagged external review reply')
  ) {
    return 'AI 返回格式异常，请重试。'
  }
  if (lower.includes('session not running') || lower.includes('no session')) {
    return '当前会话未运行，请先启动会话后重试。'
  }
  if (lower.includes('external review already pending')) {
    return '已有进行中的 AI 判断，请稍后再试。'
  }

  return truncate(toSingleLine(normalized))
}

export function parseExternalReviewSuggestions(
  text: string,
  sourceLabel: string
): ExternalReviewSuggestion[] {
  const normalized = normalizeText(text).trim()
  if (!normalized) {
    return []
  }
  return [buildSuggestion(sourceLabel, normalized, 0)]
}

export function matchSuggestionsToDiffFiles<T extends ExternalReviewDiffFile>(
  suggestions: ExternalReviewSuggestion[],
  diffFiles: T[]
): Array<ExternalReviewSuggestion & { linkedDiffFile: T | null }> {
  return suggestions.map((suggestion) => {
    if (!suggestion.pathHint) {
      return { ...suggestion, linkedDiffFile: null }
    }

    const normalizedHint = normalizePathForMatch(suggestion.pathHint)
    const exactMatch =
      diffFiles.find((diffFile) => normalizePathForMatch(diffFile.path) === normalizedHint) ?? null
    if (exactMatch) {
      return {
        ...suggestion,
        linkedDiffFile: exactMatch
      }
    }

    const suffixMatches = diffFiles.filter((diffFile) => {
      const normalizedPath = normalizePathForMatch(diffFile.path)
      return normalizedPath.endsWith(`/${normalizedHint}`)
    })

    return {
      ...suggestion,
      linkedDiffFile: suffixMatches.length === 1 ? suffixMatches[0] : null
    }
  })
}
