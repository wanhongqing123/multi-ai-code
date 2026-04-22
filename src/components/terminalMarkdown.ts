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
  const trimmed = body.trim()

  if (!trimmed) return line
  if (ANSI_ESCAPE_TEST_RE.test(body) || PATCH_LINE_RE.test(trimmed)) return line

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
  if (!MARKDOWN_HINT_RE.test(body) && !HR_RE.test(trimmed)) return line
  if (TABLE_DIVIDER_RE.test(trimmed)) return ''
  if (HR_RE.test(trimmed)) return `${wrap('────────────────────────', ANSI_DIM)}${ending}`

  const heading = body.match(HEADING_RE)
  if (heading) return `${wrap(heading[2].trim(), ANSI_BOLD)}${ending}`

  const bullet = body.match(BULLET_RE)
  if (bullet) return `${bullet[1]}• ${formatInline(bullet[2])}${ending}`

  const ordered = body.match(ORDERED_RE)
  if (ordered) return `${ordered[1]}${ordered[2]}. ${formatInline(ordered[3])}${ending}`

  if (TABLE_ROW_RE.test(body)) return `${formatTableRow(body)}${ending}`

  return `${formatInline(body)}${ending}`
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
