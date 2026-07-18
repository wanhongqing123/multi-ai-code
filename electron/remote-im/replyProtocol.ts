import { randomUUID } from 'node:crypto'

export const REMOTE_IM_REPLY_OPEN_TAG = '<remote-im-reply>'
export const REMOTE_IM_REPLY_CLOSE_TAG = '</remote-im-reply>'
const REMOTE_IM_REPLY_ID_RE = /^[A-Za-z0-9_-]{1,80}$/

export interface RemoteImAicliPromptInput {
  fromUserId: string
  text: string
  replyId?: string
}

export interface RemoteImReplyExtraction {
  content: string
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

function stripExpectedOpenMarker(line: string, replyId: string): string | null {
  const text = markerCandidateText(line)
  const exact = buildRemoteImReplyOpenTag(replyId)
  if (text.startsWith(exact)) return text.slice(exact.length)

  const malformedPrefix = `<remote-im-reply id="${replyId}`
  if (!text.startsWith(malformedPrefix)) return null
  let remainder = text.slice(malformedPrefix.length)
  if (remainder.startsWith('"')) remainder = remainder.slice(1)
  if (remainder.startsWith('>')) remainder = remainder.slice(1)
  return remainder
}

function expectedCloseMarkerIndex(line: string, replyId: string): number {
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
  const indexes = [exactIndex, legacyIndex, validMalformedIndex].filter((index) => index >= 0)
  return indexes.length > 0 ? Math.min(...indexes) : -1
}

function extractExpectedRemoteImReply(clean: string, replyId: string): RemoteImReplyExtraction {
  const replies: string[] = []
  const pendingLines: string[] = []
  let pending = false

  for (const line of clean.split('\n')) {
    const openingRemainder = stripExpectedOpenMarker(line, replyId)
    if (openingRemainder !== null) {
      pending = true
      pendingLines.length = 0
      const closeIndex = expectedCloseMarkerIndex(openingRemainder, replyId)
      if (closeIndex >= 0) {
        if (closeIndex > 0) pendingLines.push(openingRemainder.slice(0, closeIndex))
        const content = trimReplyContent(pendingLines.join('\n'))
        if (content) replies.push(content)
        pending = false
      } else if (openingRemainder) {
        pendingLines.push(openingRemainder)
      }
      continue
    }
    if (!pending) continue

    const candidate = markerCandidateText(line)
    const closeIndex = expectedCloseMarkerIndex(line, replyId)
    if (closeIndex >= 0) {
      if (closeIndex > 0) pendingLines.push(candidate.slice(0, closeIndex))
      const content = trimReplyContent(pendingLines.join('\n'))
      if (content) replies.push(content)
      pending = false
      pendingLines.length = 0
      continue
    }
    pendingLines.push(line)
  }

  return {
    content: replies.join('\n\n').trim(),
    pending,
    nextBuffer: pending ? buildPendingReplyBuffer(pendingLines, replyId) : ''
  }
}

export function buildRemoteImAicliPrompt(input: RemoteImAicliPromptInput): string {
  const replyId = normalizeReplyId(input.replyId)
  return [
    `[来自远程 IM：${input.fromUserId.trim()}]`,
    input.text,
    '',
    '如果需要查询或操作 IM，请先运行 imcli help；如需把截图或本地图片发回 IM，可保存为 png/jpg/webp/gif 文件后使用 imcli send-image <user> <imagePath>；如需发送 Markdown/HTML 报告文件，使用 imcli send-file <user> <filePath>。',
    '[IM_REPLY] Put final Markdown for IM between these exact markers, each on its own line in your reply:',
    `Opening marker: ${buildRemoteImReplyOpenTag(replyId)}`,
    `Closing marker: ${buildRemoteImReplyCloseTag(replyId)}`,
    'Text outside markers is ignored.'
  ].join('\n')
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
  let pending = false

  for (const line of clean.split('\n')) {
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
      pending = false
      pendingReplyId = undefined
      pendingLines.length = 0
      continue
    }

    pendingLines.push(line)
  }

  return {
    content: replies.join('\n\n').trim(),
    pending,
    nextBuffer: pending ? buildPendingReplyBuffer(pendingLines, pendingReplyId) : ''
  }
}
