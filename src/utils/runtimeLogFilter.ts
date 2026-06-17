export interface RuntimeLogFilterResult {
  text: string
  matchedLines: number
  totalLines: number
  active: boolean
}

export interface RuntimeLogFilterSummary {
  active: boolean
  matchedLines: number
  totalLines: number
}

function splitLogLines(log: string): string[] {
  return log.length > 0 ? log.split(/\r?\n/) : []
}

function parseFilterTerms(filter: string): string[] {
  return filter
    .trim()
    .toLowerCase()
    .split(/\s+/)
    .filter(Boolean)
}

export function filterRuntimeLogLines(log: string, filter: string): RuntimeLogFilterResult {
  const lines = splitLogLines(log)
  const terms = parseFilterTerms(filter)
  if (terms.length === 0) {
    return {
      text: log,
      matchedLines: lines.length,
      totalLines: lines.length,
      active: false,
    }
  }

  const matched = lines.filter((line) => {
    const normalized = line.toLowerCase()
    return terms.some((term) => normalized.includes(term))
  })

  return {
    text: matched.join('\n'),
    matchedLines: matched.length,
    totalLines: lines.length,
    active: true,
  }
}

export function formatRuntimeLogFilterSummary(summary: RuntimeLogFilterSummary): string {
  if (!summary.active) return `共 ${summary.totalLines} 行`
  return `匹配 ${summary.matchedLines} / ${summary.totalLines} 行`
}
