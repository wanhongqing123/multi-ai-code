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
    .replace(/\n*\u001b\[[0-9]+[HfG]/g, '\n')
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

function isPromptInstructionEcho(text: string, tagIndex: number): boolean {
  const lineStart = text.lastIndexOf('\n', tagIndex) + 1
  const lineEnd = text.indexOf('\n', tagIndex)
  const line = text.slice(lineStart, lineEnd === -1 ? text.length : lineEnd)
  return (
    line.includes('[IM_REPLY]') ||
    line.includes('Put final Markdown for IM') ||
    line.includes('text outside tags is ignored')
  )
}

export function buildRemoteImAicliPrompt(input: RemoteImAicliPromptInput): string {
  return [
    `[来自远程 IM：${input.fromUserId.trim()}]`,
    input.text,
    '',
    `[IM_REPLY] Put final Markdown for IM between full-line ${REMOTE_IM_REPLY_OPEN_TAG} and ${REMOTE_IM_REPLY_CLOSE_TAG}; text outside tags is ignored.`
  ].join('\n')
}

export function extractRemoteImReplyOutput(input: string): RemoteImReplyExtraction {
  const clean = normalizeReplyTerminalText(input)
  const replies: string[] = []
  let searchIndex = 0

  while (searchIndex < clean.length) {
    const openIndex = clean.indexOf(REMOTE_IM_REPLY_OPEN_TAG, searchIndex)
    if (openIndex === -1) break
    const contentStart = openIndex + REMOTE_IM_REPLY_OPEN_TAG.length

    if (isPromptInstructionEcho(clean, openIndex)) {
      searchIndex = contentStart
      continue
    }

    const closeIndex = clean.indexOf(REMOTE_IM_REPLY_CLOSE_TAG, contentStart)
    if (closeIndex === -1) {
      return {
        content: replies.join('\n\n').trim(),
        pending: true,
        nextBuffer: REMOTE_IM_REPLY_OPEN_TAG + clean.slice(contentStart)
      }
    }

    const content = trimReplyContent(clean.slice(contentStart, closeIndex))
    if (content) replies.push(content)
    searchIndex = closeIndex + REMOTE_IM_REPLY_CLOSE_TAG.length
  }

  return {
    content: replies.join('\n\n').trim(),
    pending: false,
    nextBuffer: ''
  }
}
