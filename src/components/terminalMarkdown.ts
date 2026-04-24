export interface TerminalMarkdownState {
  inFence: boolean
}

const ANSI_RESET = '\x1b[0m'
const ANSI_BOLD = '\x1b[1m'
const ANSI_DIM = '\x1b[2m'
const ANSI_UNDERLINE = '\x1b[4m'
const ANSI_CYAN = '\x1b[36m'

const ANSI_ESCAPE_RE = /\x1b(?:\[[0-9;?]*[ -/]*[@-~]|\][^\x07]*(?:\x07|\x1b\\))/g
const ANSI_ESCAPE_TEST_RE = /\x1b(?:\[[0-9;?]*[ -/]*[@-~]|\][^\x07]*(?:\x07|\x1b\\))/
const ANSI_SGR_RE = /^\x1b\[([0-9;]*)m$/
const PATCH_LINE_RE = /^(diff --git|index [0-9a-f]+\.\.[0-9a-f]+|--- |\+\+\+ |@@ )/
const FENCE_RE = /^\s*```/
const HEADING_RE = /^\s{0,3}(#{1,6})\s+(.+?)\s*$/
const BULLET_RE = /^(\s*)[-*+]\s+(.+?)\s*$/
const ORDERED_RE = /^(\s*)(\d+)\.\s+(.+?)\s*$/
const TABLE_ROW_RE = /^\s*\|.*\|\s*$/
const TABLE_DIVIDER_RE = /^\s*\|(?:\s*:?-{3,}:?\s*\|)+\s*$/
const HR_RE = /^\s*([-*_])(?:\s*\1){2,}\s*$/
const MARKDOWN_HINT_RE =
  /(^\s{0,3}#{1,6}\s+)|(^\s*[-*+]\s+)|(^\s*\d+\.\s+)|(`[^`\n]+`)|(\*\*[^*\n]+\*\*)|(\[[^\]]+\]\([^)]+\))|(^\s*\|.*\|\s*$)|(^\s*```)/m

function wrap(text: string, code: string): string {
  if (!text) return text
  return `${code}${text}${ANSI_RESET}`
}

function wrapPreservingInnerResets(text: string, code: string): string {
  if (!text) return text
  return `${code}${text.replaceAll(ANSI_RESET, `${ANSI_RESET}${code}`)}${ANSI_RESET}`
}

function splitLineEnding(line: string): { body: string; ending: string } {
  if (line.endsWith('\r\n')) return { body: line.slice(0, -2), ending: '\r\n' }
  if (line.endsWith('\n') || line.endsWith('\r')) {
    return { body: line.slice(0, -1), ending: line.slice(-1) }
  }
  return { body: line, ending: '' }
}

function splitChunk(chunk: string): { complete: string[]; trailing: string } {
  const parts = chunk.match(/[^\r\n]*(?:\r\n|\n|\r)|[^\r\n]+$/g) ?? []
  const complete: string[] = []
  let trailing = ''
  for (const part of parts) {
    if (part.endsWith('\n') || part.endsWith('\r')) complete.push(part)
    else trailing = part
  }
  return { complete, trailing }
}

function hasOnlySafeSgrAnsi(text: string): boolean {
  const matches = text.match(ANSI_ESCAPE_RE) ?? []
  return matches.every((seq) => {
    const sgr = seq.match(ANSI_SGR_RE)
    if (!sgr) return false
    const rawParams = sgr[1]
    const params = rawParams.length === 0 ? [0] : rawParams.split(';').map((v) => Number(v || 0))
    for (let i = 0; i < params.length; i++) {
      const p = params[i]
      if (Number.isNaN(p)) return false
      if (
        p === 0 ||
        p === 1 ||
        p === 2 ||
        p === 22 ||
        p === 4 ||
        p === 24 ||
        p === 39 ||
        (p >= 30 && p <= 37) ||
        (p >= 90 && p <= 97)
      ) {
        continue
      }
      if (p === 38) {
        const mode = params[i + 1]
        if (mode === 5) {
          i += 2
          continue
        }
        if (mode === 2) {
          i += 4
          continue
        }
      }
      return false
    }
    return true
  })
}

function visibleSlice(raw: string, start: number, end: number): string {
  if (end <= start) return ''
  const matches = raw.match(ANSI_ESCAPE_RE) ?? []
  const tokens = raw.split(ANSI_ESCAPE_RE)
  let out = ''
  let visible = 0
  let pendingAnsi = ''

  for (let i = 0; i < tokens.length; i++) {
    const text = tokens[i] ?? ''
    for (const ch of Array.from(text)) {
      if (visible >= start && visible < end) {
        if (pendingAnsi) {
          out += pendingAnsi
          pendingAnsi = ''
        }
        out += ch
      }
      visible += 1
    }
    const ansi = matches[i] ?? ''
    if (!ansi) continue
    if (visible >= start && visible < end) out += ansi
    else pendingAnsi += ansi
  }

  return out
}

function formatInline(text: string): string {
  return text
    .replace(/\[([^\]]+)\]\(([^)]+)\)/g, (_all, label: string, url: string) => {
      return `${wrap(label, ANSI_UNDERLINE)} ${wrap(`(${url})`, ANSI_DIM)}`
    })
    .replace(/\*\*([^*\n]+)\*\*/g, (_all, bold: string) => {
      return wrap(bold, ANSI_BOLD)
    })
    .replace(/`([^`\n]+)`/g, (_all, code: string) => {
      return wrap(code, ANSI_CYAN)
    })
}

function formatInlineMixed(raw: string, plain: string): string {
  const pattern =
    /\[([^\]]+)\]\(([^)]+)\)|\*\*([^*\n]+)\*\*|`([^`\n]+)`/g
  let out = ''
  let cursor = 0

  for (const match of plain.matchAll(pattern)) {
    const start = match.index ?? 0
    const end = start + match[0].length
    out += visibleSlice(raw, cursor, start)
    if (match[1] != null && match[2] != null) {
      const labelStart = start + 1
      const labelEnd = labelStart + match[1].length
      out += wrapPreservingInnerResets(
        visibleSlice(raw, labelStart, labelEnd),
        ANSI_UNDERLINE
      )
      out += ` ${wrap(`(${match[2]})`, ANSI_DIM)}`
    } else if (match[3] != null) {
      const innerStart = start + 2
      const innerEnd = innerStart + match[3].length
      out += wrapPreservingInnerResets(
        visibleSlice(raw, innerStart, innerEnd),
        ANSI_BOLD
      )
    } else if (match[4] != null) {
      const innerStart = start + 1
      const innerEnd = innerStart + match[4].length
      out += wrapPreservingInnerResets(
        visibleSlice(raw, innerStart, innerEnd),
        ANSI_CYAN
      )
    }
    cursor = end
  }

  out += visibleSlice(raw, cursor, plain.length)
  return out
}

function formatTableRow(body: string): string {
  const cells = body
    .trim()
    .slice(1, -1)
    .split('|')
    .map((cell) => formatInline(cell.trim()))
  return cells.join(wrap(' │ ', ANSI_DIM))
}

function formatMarkdownLine(
  line: string,
  state: TerminalMarkdownState
): string {
  const { body, ending } = splitLineEnding(line)
  const plainBody = stripAnsi(body)
  const trimmed = plainBody.trim()
  const hasAnsi = ANSI_ESCAPE_TEST_RE.test(body)

  if (!trimmed) return line
  if (hasAnsi && !hasOnlySafeSgrAnsi(body)) return line
  if (PATCH_LINE_RE.test(trimmed)) return line

  if (FENCE_RE.test(trimmed)) {
    const lang = trimmed.replace(/^\s*```/, '').trim()
    if (state.inFence) {
      state.inFence = false
      return `${wrap('└─', ANSI_DIM)}${ending}`
    }
    state.inFence = true
    return `${wrap(lang ? `┌─ ${lang}` : '┌─ code', ANSI_DIM)}${ending}`
  }

  if (state.inFence) return line
  if (!MARKDOWN_HINT_RE.test(plainBody) && !HR_RE.test(trimmed)) return line
  if (TABLE_DIVIDER_RE.test(trimmed)) return ''
  if (HR_RE.test(trimmed)) return `${wrap('────────────────────────', ANSI_DIM)}${ending}`

  const heading = plainBody.match(HEADING_RE)
  if (heading) {
    if (!hasAnsi) return `${wrap(heading[2].trim(), ANSI_BOLD)}${ending}`
    const contentStart = heading[0].length - heading[2].length
    return `${wrapPreservingInnerResets(visibleSlice(body, contentStart, plainBody.length).trim(), ANSI_BOLD)}${ending}`
  }

  const bullet = plainBody.match(BULLET_RE)
  if (bullet) {
    const prefixLen = bullet[0].length - bullet[2].length
    if (!hasAnsi) return `${bullet[1]}• ${formatInline(bullet[2])}${ending}`
    return `${visibleSlice(body, 0, bullet[1].length)}• ${formatInlineMixed(
      visibleSlice(body, prefixLen, plainBody.length),
      bullet[2]
    )}${ending}`
  }

  const ordered = plainBody.match(ORDERED_RE)
  if (ordered) {
    const prefixLen = ordered[0].length - ordered[3].length
    if (!hasAnsi) {
      return `${ordered[1]}${ordered[2]}. ${formatInline(ordered[3])}${ending}`
    }
    return `${visibleSlice(body, 0, ordered[1].length)}${ordered[2]}. ${formatInlineMixed(
      visibleSlice(body, prefixLen, plainBody.length),
      ordered[3]
    )}${ending}`
  }

  if (TABLE_ROW_RE.test(plainBody)) return `${formatTableRow(plainBody)}${ending}`

  return hasAnsi
    ? `${formatInlineMixed(body, plainBody)}${ending}`
    : `${formatInline(body)}${ending}`
}

export function formatMarkdownChunk(
  chunk: string,
  state: TerminalMarkdownState
): { text: string; state: TerminalMarkdownState } {
  const { complete, trailing } = splitChunk(chunk)
  let text = ''
  for (const line of complete) {
    text += formatMarkdownLine(line, state)
  }
  text += trailing
  return { text, state }
}

export function createTerminalMarkdownState(): TerminalMarkdownState {
  return { inFence: false }
}

export function stripAnsi(text: string): string {
  return text.replace(ANSI_ESCAPE_RE, '')
}
