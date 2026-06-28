import { stripTerminalControl } from './outputBuffer.js'

const BORDER_ONLY_RE = /^[\s─━│┃┌┐└┘├┤┬┴┼╭╮╰╯╔╗╚╝╠╣╦╩╬═║]+$/
const THINKING_WITH_EFFORT_RE = /(?:thinking|reasoning)\s+with\s+[\w-]+\s+effort/i
const COGITATED_RE = /\bCogitated\s+for\s+\d+(?:\.\d+)?s\b/i
const STATUS_COUNTER_RE =
  /(?:\bctx\s+\d+%\/[\w.]+|\b(?:in|out)\s+\d+(?:\.\d+)?[KMG]?\b|\/\s*out\s+\d+|\b\d+d\s+\d+%|\b\d+h\s+\d+%)/i
const SPINNER_RE = /\b(?:Newspapering|Warping|Cogitating|Thinking|Reasoning)\b/i
const PROMPT_HINT_RE =
  /(?:ctrl\+g to edit|Press up to edit queued messages|← for agents|^\s*❯\s*$)/i
const TEMP_PATH_RE = /AppData\\Local\\Temp\\multi-ai-code-mutual/i
const CLAUDE_BOOT_RE =
  /(?:Welcome back|What's new|Claude Max|Opus\s+\d+(?:\.\d+)?|release-notes|OTEL_LOG_ASSISTANT_RESPONS)/i
const REMOTE_IM_PROMPT_ECHO_RE = /\[\u6765\u81ea\u8fdc\u7a0b\s*IM[\uFF1A:][^\]\n]*\]/
const CLAUDE_ASSISTANT_MARKER_RE = /[\u25CF\u2022]\s*/
const LEADING_CLAUDE_ASSISTANT_MARKER_RE = /^\s*[\u25CF\u2022]\s*/

function stripRemoteImPromptEcho(line: string): string {
  const match = REMOTE_IM_PROMPT_ECHO_RE.exec(line)
  if (!match) return line

  const afterPrompt = line.slice(match.index + match[0].length)
  const assistantMarker = CLAUDE_ASSISTANT_MARKER_RE.exec(afterPrompt)
  if (assistantMarker) {
    return afterPrompt.slice(assistantMarker.index + assistantMarker[0].length)
  }

  return ''
}

function normalizeContentLine(line: string): string {
  return stripRemoteImPromptEcho(line).replace(LEADING_CLAUDE_ASSISTANT_MARKER_RE, '')
}

function isTerminalNoiseLine(line: string): boolean {
  const text = line.trim()
  if (!text) return false
  if (BORDER_ONLY_RE.test(text)) return true
  if (THINKING_WITH_EFFORT_RE.test(text)) return true
  if (COGITATED_RE.test(text)) return true
  if (STATUS_COUNTER_RE.test(text) && /(?:ctx|token|cache|total|otal|\|)/i.test(text)) return true
  if (SPINNER_RE.test(text) && /[✢✶✻✽…❯]/.test(text)) return true
  if (PROMPT_HINT_RE.test(text)) return true
  if (TEMP_PATH_RE.test(text)) return true
  if (CLAUDE_BOOT_RE.test(text)) return true
  return false
}

function normalizeBlankLines(lines: string[]): string {
  const out: string[] = []
  for (const line of lines) {
    if (line.trim()) {
      out.push(line.trimEnd())
      continue
    }
    if (out.length > 0 && out[out.length - 1] !== '') out.push('')
  }
  while (out.length > 0 && out[out.length - 1] === '') out.pop()
  return out.join('\n').trim()
}

export function sanitizeRemoteImAicliOutput(input: string): string {
  const clean = stripTerminalControl(input)
  const lines = clean
    .split('\n')
    .map((line) => normalizeContentLine(line))
    .filter((line) => !isTerminalNoiseLine(line))
  return normalizeBlankLines(lines)
}
