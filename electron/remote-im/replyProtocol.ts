export const REMOTE_IM_REPLY_OPEN_TAG = '<remote-im-reply>'
export const REMOTE_IM_REPLY_CLOSE_TAG = '</remote-im-reply>'

export interface RemoteImAicliPromptInput {
  fromUserId: string
  text: string
}

export interface RemoteImReplyExtraction {
  content: string
  pending: boolean
  nextBuffer: string
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

function isTagLine(line: string, tag: string): boolean {
  return line.trim().replace(/^[\u23fa\u25CF\u2022]\s*/, '').trim() === tag
}

function buildPendingReplyBuffer(lines: string[]): string {
  return [REMOTE_IM_REPLY_OPEN_TAG, ...lines].join('\n')
}

export function buildRemoteImAicliPrompt(input: RemoteImAicliPromptInput): string {
  return [
    `[来自远程 IM：${input.fromUserId.trim()}]`,
    input.text,
    '',
    '如果需要查询或操作 IM，请先运行 imcli help。',
    '[IM_REPLY] Put final Markdown for IM between these full-line markers:',
    REMOTE_IM_REPLY_OPEN_TAG,
    REMOTE_IM_REPLY_CLOSE_TAG,
    'Text outside markers is ignored.'
  ].join('\n')
}

export function buildRemoteImAicliDisplayText(input: RemoteImAicliPromptInput): string {
  return [`[来自远程 IM：${input.fromUserId.trim()}]`, input.text].join('\n').trim()
}

export function extractRemoteImReplyOutput(input: string): RemoteImReplyExtraction {
  const clean = normalizeReplyTerminalText(input)
  const replies: string[] = []
  const pendingLines: string[] = []
  let pending = false

  for (const line of clean.split('\n')) {
    if (isTagLine(line, REMOTE_IM_REPLY_OPEN_TAG)) {
      pending = true
      pendingLines.length = 0
      continue
    }

    if (!pending) {
      continue
    }

    if (isTagLine(line, REMOTE_IM_REPLY_CLOSE_TAG)) {
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
    nextBuffer: pending ? buildPendingReplyBuffer(pendingLines) : ''
  }
}
