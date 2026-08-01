// Mirror of electron/orchestrator/session-messages.ts - renderer-safe copy.
// Keep in sync manually when the backend copy changes.

export interface SessionAnnotation {
  readonly file: string
  readonly lineRange: string
  readonly snippet: string
  readonly comment: string
}

function sanitize(label: string): string {
  return label
    .trim()
    .replace(/[<>:"/\\|?*\x00-\x1f]/g, '_')
    .replace(/\s+/g, '_')
    .slice(0, 80)
}

export function planNameToFilename(name: string): string {
  const safe = name && name.trim() ? sanitize(name) : 'design'
  return `${safe}.md`
}

export interface InitialMessageParams {
  readonly planName: string
  readonly planAbsPath: string
  readonly planExists: boolean
}

export function formatInitialMessage(p: InitialMessageParams): string {
  if (p.planExists) {
    return [
      `本次方案名：${p.planName}。`,
      '',
      `当前方案文件：\`${p.planAbsPath}\`。`,
      '',
      '请先阅读当前方案文件，用中文简要总结：目标、核心步骤、预期产物。',
      '',
      '**此时不要修改任何代码或方案文件**。等用户确认方向（或让你按方案实施）后，再继续执行。'
    ].join('\n')
  }

  return [
    `本次方案名：${p.planName}。`,
    '',
    `请先与用户对话澄清需求、确认方向，然后把方案写到 \`${p.planAbsPath}\`（完整绝对路径），再继续实施。`
  ].join('\n')
}

export interface AnnotationsForSessionParams {
  readonly annotations: readonly SessionAnnotation[]
  readonly generalComment: string
  /** 当前任务的方案文件。没有任务在跑时留空——批注本身照样成立。 */
  readonly planAbsPath?: string
}

export function formatAnnotationsForSession(
  p: AnnotationsForSessionParams
): string {
  const planAbsPath = p.planAbsPath?.trim() ?? ''
  const lines: string[] = []
  lines.push('# 用户批注')
  lines.push('')
  // 没有方案文件就别提它。以前这里硬拼一个路径，于是「必须先选中一个普通任务」
  // 成了发批注的前置条件——可批注针对的是代码改动，跟有没有任务无关。
  lines.push(
    planAbsPath
      ? `以下是用户对当前改动的批注，请严格按照批注执行：修改代码、或更新方案文档（\`${planAbsPath}\`）。`
      : '以下是用户对当前改动的批注，请严格按照批注执行，直接修改代码。'
  )
  lines.push('')
  lines.push('## 逐行批注')
  lines.push('')

  for (const a of p.annotations) {
    lines.push('### 批注')
    lines.push('')
    lines.push(`文件: ${a.file}`)
    lines.push(`行号: ${a.lineRange}`)
    lines.push('')
    lines.push('代码片段:')
    lines.push('')
    lines.push('```')
    lines.push(a.snippet)
    lines.push('```')
    lines.push('')
    lines.push('批注:')
    lines.push('')
    lines.push(a.comment)
    lines.push('')
  }

  const gc = p.generalComment.trim()
  if (gc.length > 0) {
    lines.push('## 整体意见')
    lines.push('')
    lines.push(gc)
    lines.push('')
  }

  lines.push('---')
  lines.push('')
  lines.push(
    planAbsPath
      ? '请按照以上批注调整代码 / 方案，完成后在终端里简述改了什么。'
      : '请按照以上批注调整代码，完成后在终端里简述改了什么。'
  )
  return lines.join('\n')
}
