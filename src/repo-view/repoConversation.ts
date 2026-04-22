export interface RepoConversationMessage {
  id: string
  role: 'user' | 'assistant'
  text: string
  streaming?: boolean
}

function genMessageId(prefix: string): string {
  return `${prefix}_${Date.now().toString(36)}_${Math.random().toString(36).slice(2, 7)}`
}

export function buildRepoUserMessageText(input: {
  filePath: string
  annotationCount: number
  question: string
}): string {
  const lines = [
    `文件：${input.filePath}`,
    `标注：${input.annotationCount} 条标注`
  ]
  if (input.question.trim()) {
    lines.push(`要求：${input.question.trim()}`)
  } else {
    lines.push('要求：请按标注说明优先分析；如果说明明确要求修改代码，就直接修改并解释改动。')
  }
  return lines.join('\n')
}

export function createUserMessage(input: {
  filePath: string
  annotationCount: number
  question: string
}): RepoConversationMessage {
  return {
    id: genMessageId('user'),
    role: 'user',
    text: buildRepoUserMessageText(input)
  }
}

export function syncAssistantMessage(
  messages: RepoConversationMessage[],
  text: string,
  streaming: boolean
): RepoConversationMessage[] {
  const next = [...messages]
  const last = next[next.length - 1]
  if (last?.role === 'assistant') {
    next[next.length - 1] = { ...last, text, streaming }
    return next
  }
  next.push({
    id: genMessageId('assistant'),
    role: 'assistant',
    text,
    streaming
  })
  return next
}
