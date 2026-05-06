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
}

const BULLET_LINE_RE = /^(\s*)(?:[-*]|\d+\.)\s+(.*)$/
const HEADING_LINE_RE = /^\s*#{1,6}\s+\S/
const PATH_HINT_RE = /(?:\.{1,2}[\\/])?[A-Za-z0-9_.-]+(?:[\\/][A-Za-z0-9_.-]+)+\.[A-Za-z0-9]+/g

function normalizeText(text: string): string {
  return text.replace(/\r\n?/g, '\n')
}

function isHeadingOnlyBlock(block: string): boolean {
  const lines = block
    .split('\n')
    .map((line) => line.trim())
    .filter(Boolean)

  return lines.length > 0 && lines.every((line) => HEADING_LINE_RE.test(line))
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
    decisionReason: ''
  }
}

function splitBulletBlocks(text: string): string[] {
  const lines = normalizeText(text).split('\n')
  const blocks: string[] = []
  let current: string[] = []
  let sawBullet = false

  const pushCurrent = () => {
    const block = current.join('\n').trim()
    if (block && !isHeadingOnlyBlock(block)) {
      blocks.push(block)
    }
    current = []
  }

  for (const line of lines) {
    const bulletMatch = line.match(BULLET_LINE_RE)
    if (bulletMatch) {
      sawBullet = true
      if (current.length > 0) {
        pushCurrent()
      }
      current = [bulletMatch[2].trimEnd()]
      continue
    }

    if (!sawBullet) {
      continue
    }

    if (current.length === 0 && !line.trim()) {
      continue
    }

    current.push(line)
  }

  if (current.length > 0) {
    pushCurrent()
  }

  return sawBullet ? blocks : []
}

function splitParagraphBlocks(text: string): string[] {
  return normalizeText(text)
    .split(/\n\s*\n+/)
    .map((block) => block.trim())
    .filter((block) => block.length > 0 && !isHeadingOnlyBlock(block))
}

function normalizePathForMatch(path: string): string {
  return path.replace(/\\/g, '/').trim()
}

export function parseExternalReviewSuggestions(
  text: string,
  sourceLabel: string
): ExternalReviewSuggestion[] {
  const normalized = normalizeText(text).trim()
  if (!normalized) {
    return []
  }

  const bulletBlocks = splitBulletBlocks(normalized)
  const blocks = bulletBlocks.length > 0 ? bulletBlocks : splitParagraphBlocks(normalized)

  return blocks.map((rawText, index) => buildSuggestion(sourceLabel, rawText, index))
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
