import { randomUUID } from 'node:crypto'

export const REMOTE_IM_REPLY_OPEN_TAG = '<remote-im-reply>'
export const REMOTE_IM_REPLY_CLOSE_TAG = '</remote-im-reply>'
const REMOTE_IM_REPLY_ID_RE = /^[A-Za-z0-9_-]{1,80}$/

export interface RemoteImAicliPromptInput {
  fromUserId: string
  text: string
  replyId?: string
}

export interface RemoteImAicliPromptOptions {
  includeReplyProtocol?: boolean
}

export interface RemoteImReplyExtraction {
  content: string
  /**
   * The envelope is syntactically closed. Callers that use this as route
   * authority must additionally establish that the text came from an
   * assistant-authored source rather than terminal echo.
   */
  completed: boolean
  pending: boolean
  nextBuffer: string
}

export interface RemoteImReplyExtractionOptions {
  replyId?: string
}

interface RemoteImReplyTag {
  kind: 'open' | 'close'
  replyId?: string
}

export function createRemoteImReplyId(): string {
  return `rim-${randomUUID().replace(/-/g, '').slice(0, 16)}`
}

function normalizeReplyId(replyId: string | undefined): string | undefined {
  const trimmed = replyId?.trim()
  return trimmed && REMOTE_IM_REPLY_ID_RE.test(trimmed) ? trimmed : undefined
}

export function buildRemoteImReplyOpenTag(replyId?: string): string {
  const normalized = normalizeReplyId(replyId)
  return normalized ? `<remote-im-reply id="${normalized}">` : REMOTE_IM_REPLY_OPEN_TAG
}

export function buildRemoteImReplyCloseTag(replyId?: string): string {
  const normalized = normalizeReplyId(replyId)
  return normalized ? `</remote-im-reply id="${normalized}">` : REMOTE_IM_REPLY_CLOSE_TAG
}

function normalizeReplyTerminalText(input: string): string {
  return input
    .replace(/\u001b\][^\u0007]*(\u0007|\u001b\\)/g, '')
    .replace(/\r\n/g, '\n')
    .replace(/\r/g, '\n')
    .replace(/\n*\u001b\[[0-9]+;[0-9]+[Hf]/g, '\n')
    .replace(/\n*\u001b\[[0-9]+[Hf]/g, '\n')
    .replace(/(\n*)\u001b\[[0-9]+G/g, (_match, lineBreaks: string) => (lineBreaks ? '\n' : ' '))
    .replace(/\u001b\[(\d+)C/g, ' ')
    .replace(/\u001b\[[0-?]*[ -/]*[@-~]/g, '')
    .replace(/\u001b[=>]/g, '')
}

function trimReplyContent(input: string): string {
  const out = input
    .split('\n')
    .map((line) => line.replace(/^ {1,2}/, '').trimEnd())
  while (out.length > 0 && !out[0].trim()) out.shift()
  while (out.length > 0 && !out[out.length - 1].trim()) out.pop()
  return out.join('\n').trim()
}

function parseTagLine(line: string): RemoteImReplyTag | null {
  const text = line.trim().replace(/^[\u23fa\u25CF\u2022]\s*/, '').trim()
  if (text === REMOTE_IM_REPLY_OPEN_TAG) return { kind: 'open' }
  if (text === REMOTE_IM_REPLY_CLOSE_TAG) return { kind: 'close' }
  const open = /^<remote-im-reply\s+id="([A-Za-z0-9_-]+)">$/.exec(text)
  if (open) return { kind: 'open', replyId: normalizeReplyId(open[1]) }
  const close = /^<\/remote-im-reply\s+id="([A-Za-z0-9_-]+)">$/.exec(text)
  if (close) return { kind: 'close', replyId: normalizeReplyId(close[1]) }
  return null
}

function matchesExpectedReplyId(tag: RemoteImReplyTag, expectedReplyId: string | undefined): boolean {
  return expectedReplyId ? tag.replyId === expectedReplyId : true
}

function matchesPendingCloseTag(
  tag: RemoteImReplyTag,
  expectedReplyId: string | undefined,
  pendingReplyId: string | undefined
): boolean {
  if (tag.kind !== 'close') return false
  if (!expectedReplyId) return true
  if (tag.replyId === expectedReplyId && tag.replyId === pendingReplyId) return true
  return tag.replyId === undefined && pendingReplyId === expectedReplyId
}

function buildPendingReplyBuffer(lines: string[], replyId?: string): string {
  return [buildRemoteImReplyOpenTag(replyId), ...lines].join('\n')
}

function markerCandidateText(line: string): string {
  return line.trimStart().replace(/^[\u23fa\u25CF\u2022]\s*/, '').trimStart()
}

// The injected prompt echoes "Opening marker: <tag>" / "Closing marker: </tag>" lines back
// through the TUI. A terminal redraw can split such a line so the bare tag lands at a line
// start and false-opens a reply; the indexOf-based close detection then matches the example
// close tag on the next echoed line, forwarding a "Closing marker:" fragment while the real
// reply gets dropped by forwardedReplyId dedupe. Skip these lines entirely: they never
// open/close a reply and never count as reply content.
const PROMPT_MARKER_INSTRUCTION_LINE_RE = /^(?:Opening|Closing)\s+marker\s*[:\uff1a]/i

function isPromptMarkerInstructionLine(line: string): boolean {
  return PROMPT_MARKER_INSTRUCTION_LINE_RE.test(markerCandidateText(line))
}

interface ExpectedMarkerMatch {
  index: number
  exact: boolean
}

interface ExpectedOpenMarkerMatch {
  remainder: string
  exact: boolean
}

function stripExpectedOpenMarker(line: string, replyId: string): ExpectedOpenMarkerMatch | null {
  const text = markerCandidateText(line)
  const exact = buildRemoteImReplyOpenTag(replyId)
  if (text.startsWith(exact)) {
    return { remainder: text.slice(exact.length), exact: true }
  }

  const malformedPrefix = `<remote-im-reply id="${replyId}`
  if (!text.startsWith(malformedPrefix)) return null
  let remainder = text.slice(malformedPrefix.length)
  if (remainder.startsWith('"')) remainder = remainder.slice(1)
  if (remainder.startsWith('>')) remainder = remainder.slice(1)
  return { remainder, exact: false }
}

function expectedCloseMarkerMatch(line: string, replyId: string): ExpectedMarkerMatch | null {
  const text = markerCandidateText(line)
  const exactIndex = text.indexOf(buildRemoteImReplyCloseTag(replyId))
  const legacyIndex = text.indexOf(REMOTE_IM_REPLY_CLOSE_TAG)
  const malformedPrefix = `</remote-im-reply id="${replyId}`
  const malformedIndex = text.indexOf(malformedPrefix)
  const validMalformedIndex =
    malformedIndex >= 0 &&
    (() => {
      const tail = text.slice(malformedIndex + malformedPrefix.length)
      return !tail || /^[">\s]/.test(tail)
    })()
      ? malformedIndex
      : -1
  const matches = [
    exactIndex >= 0 ? { index: exactIndex, exact: true } : null,
    legacyIndex >= 0 ? { index: legacyIndex, exact: false } : null,
    validMalformedIndex >= 0 ? { index: validMalformedIndex, exact: false } : null
  ].filter((match): match is ExpectedMarkerMatch => match !== null)
  return matches.sort((a, b) => a.index - b.index)[0] ?? null
}

function extractExpectedRemoteImReply(clean: string, replyId: string): RemoteImReplyExtraction {
  const replies: string[] = []
  const pendingLines: string[] = []
  let completed = false
  let pending = false
  let pendingExactOpen = false

  for (const line of clean.split('\n')) {
    if (isPromptMarkerInstructionLine(line)) continue
    const opening = stripExpectedOpenMarker(line, replyId)
    if (opening !== null) {
      pending = true
      pendingExactOpen = opening.exact
      pendingLines.length = 0
      const close = expectedCloseMarkerMatch(opening.remainder, replyId)
      if (close) {
        if (close.index > 0) pendingLines.push(opening.remainder.slice(0, close.index))
        const content = trimReplyContent(pendingLines.join('\n'))
        if (content) replies.push(content)
        if (opening.exact && close.exact) completed = true
        pending = false
        pendingExactOpen = false
      } else if (opening.remainder) {
        pendingLines.push(opening.remainder)
      }
      continue
    }
    if (!pending) continue

    const candidate = markerCandidateText(line)
    const close = expectedCloseMarkerMatch(line, replyId)
    if (close) {
      if (close.index > 0) pendingLines.push(candidate.slice(0, close.index))
      const content = trimReplyContent(pendingLines.join('\n'))
      if (content) replies.push(content)
      if (pendingExactOpen && close.exact) completed = true
      pending = false
      pendingExactOpen = false
      pendingLines.length = 0
      continue
    }
    pendingLines.push(line)
  }

  return {
    content: replies.join('\n\n').trim(),
    completed,
    pending,
    nextBuffer: pending ? buildPendingReplyBuffer(pendingLines, replyId) : ''
  }
}

export function buildRemoteImAicliPrompt(
  input: RemoteImAicliPromptInput,
  options: RemoteImAicliPromptOptions = {}
): string {
  const lines = [
    `[来自远程 IM：${input.fromUserId.trim()}]`,
    input.text,
    '',
    '如果需要查询或操作 IM，请先运行 imcli help；如需把截图或本地图片发回 IM，可保存为 png/jpg/webp/gif 文件后使用 imcli send-image <user> <imagePath>；如需发送 Markdown/HTML 报告文件，使用 imcli send-file <user> <filePath>；如需发送当前仓库的代码 Diff，使用 imcli send-diff <user> [--working | --commit <ref> | --range <base>..<head>]。正常回复必须使用真实的 Markdown 换行，不要把 Windows 命令行的转义规则用于回复正文。手工调用 imcli send 发送文本时，正文一律传 UTF-8 文本的标准 Base64：imcli send <user> --text-b64 <base64>。'
  ]
  if (options.includeReplyProtocol === false) return lines.join('\n')
  const replyId = normalizeReplyId(input.replyId)
  lines.push(
    '[IM_REPLY] Put final Markdown for IM between these exact markers, each on its own line in your reply:',
    `Opening marker: ${buildRemoteImReplyOpenTag(replyId)}`,
    `Closing marker: ${buildRemoteImReplyCloseTag(replyId)}`,
    'Text outside markers is ignored.'
  )
  return lines.join('\n')
}

export function buildRemoteImAicliDisplayText(input: RemoteImAicliPromptInput): string {
  return [`[来自远程 IM：${input.fromUserId.trim()}]`, input.text].join('\n').trim()
}

export function extractRemoteImReplyOutput(
  input: string,
  options: RemoteImReplyExtractionOptions = {}
): RemoteImReplyExtraction {
  const clean = normalizeReplyTerminalText(input)
  const replies: string[] = []
  const pendingLines: string[] = []
  const expectedReplyId = normalizeReplyId(options.replyId)
  if (expectedReplyId) return extractExpectedRemoteImReply(clean, expectedReplyId)
  let pendingReplyId: string | undefined
  let completed = false
  let pending = false

  for (const line of clean.split('\n')) {
    if (isPromptMarkerInstructionLine(line)) continue
    const tag = parseTagLine(line)
    if (tag?.kind === 'open') {
      if (matchesExpectedReplyId(tag, expectedReplyId)) {
        pending = true
        pendingReplyId = tag.replyId
        pendingLines.length = 0
      }
      continue
    }

    if (!pending) {
      continue
    }

    if (
      tag?.kind === 'close' &&
      matchesPendingCloseTag(tag, expectedReplyId, pendingReplyId)
    ) {
      const content = trimReplyContent(pendingLines.join('\n'))
      if (content) replies.push(content)
      completed = true
      pending = false
      pendingReplyId = undefined
      pendingLines.length = 0
      continue
    }

    pendingLines.push(line)
  }

  return {
    content: replies.join('\n\n').trim(),
    completed,
    pending,
    nextBuffer: pending ? buildPendingReplyBuffer(pendingLines, pendingReplyId) : ''
  }
}
