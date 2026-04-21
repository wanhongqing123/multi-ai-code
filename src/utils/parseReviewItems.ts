export type Severity = 'must-fix' | 'suggestion'

export interface ReviewItem {
  id: number
  severity: Severity
  location: string
  /** Full markdown body below the heading line */
  body: string
  /** Best-effort title extracted from `**标题**:` line if present */
  title: string
}

const ITEM_HEAD_RE = /^##\s+Item\s+(\d+)\s*[·•|-]\s*(must-fix|suggestion)\s*[·•|-]\s*(.+?)\s*$/i
const TITLE_LINE_RE = /^\*\*(?:标题|Title)\*\*\s*[:：]\s*(.+?)\s*$/m

/**
 * Parse a Stage 4 review markdown into discrete items.
 * Falls back to an empty array if no items match the strict format.
 */
export function parseReviewItems(md: string): ReviewItem[] {
  if (!md) return []
  const lines = md.split(/\r?\n/)
  const items: ReviewItem[] = []
  let current: { id: number; severity: Severity; location: string; body: string[] } | null =
    null

  const flush = () => {
    if (current) {
      const body = current.body.join('\n').trim()
      const titleMatch = body.match(TITLE_LINE_RE)
      items.push({
        id: current.id,
        severity: current.severity,
        location: current.location,
        body,
        title: titleMatch ? titleMatch[1] : `Item ${current.id}`
      })
    }
  }

  for (const line of lines) {
    const m = line.match(ITEM_HEAD_RE)
    if (m) {
      flush()
      current = {
        id: Number(m[1]),
        severity: m[2].toLowerCase() as Severity,
        location: m[3],
        body: []
      }
    } else if (current) {
      current.body.push(line)
    }
  }
  flush()
  return items
}

export function buildSelectedFeedback(items: ReviewItem[]): string {
  if (items.length === 0) return ''
  const lines = [
    `# Code Review 修复清单（共 ${items.length} 项）`,
    '',
    '以下是 Reviewer 提出、用户审批通过的问题，请逐项修复。修复完毕后在终端简述改了什么。',
    ''
  ]
  for (const it of items) {
    lines.push(
      `## Item ${it.id} · ${it.severity} · ${it.location}`,
      it.body,
      ''
    )
  }
  return lines.join('\n')
}
