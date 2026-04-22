export interface SessionAnnotation {
  /** Relative path of the annotated file (from target_repo root). */
  file: string
  /** "10" or "10-12" — line number or inclusive range. */
  lineRange: string
  /** The exact code snippet the user highlighted. */
  snippet: string
  /** User's comment on this location. Rendered verbatim as markdown body —
   *  callers should trust only in-app UI input (not external paste). */
  comment: string
}

export interface InitialMessageParams {
  /** Plan name as the user typed it (e.g. "add-auth"). */
  planName: string
  /** Absolute path where the plan markdown lives (or should be written). */
  planAbsPath: string
  /** Plan file content if it already exists on disk; null for new plans. */
  planContent: string | null
}

export function formatInitialMessage(p: InitialMessageParams): string {
  if (p.planContent !== null && p.planContent.trim().length > 0) {
    return [
      p.planContent.trimEnd(),
      '',
      '---',
      '',
      '请先阅读当前方案，用中文简要总结：目标、核心步骤、预期产物。',
      '',
      '**此时不要修改任何代码或方案文档**。等用户确认方向（或让你按方案实施）后，再继续执行。'
    ].join('\n')
  }
  return [
    `本次方案名：${p.planName}。`,
    '',
    `请先与用户对话澄清需求、确认方向，然后把方案写到 \`${p.planAbsPath}\`（完整绝对路径），再继续实施。`
  ].join('\n')
}

export interface AnnotationsForSessionParams {
  annotations: SessionAnnotation[]
  /** User's optional overall comment on the whole diff. Rendered verbatim
   *  as markdown body — callers should trust only in-app UI input. */
  generalComment: string
  /** Absolute path of the current plan markdown (for "update plan if asked" reference). */
  planAbsPath: string
}

export function formatAnnotationsForSession(
  p: AnnotationsForSessionParams
): string {
  const lines: string[] = []
  lines.push('# 用户批注')
  lines.push('')
  lines.push(
    `以下是用户对当前改动的批注，请严格按照批注执行：修改代码、或更新方案文档（\`${p.planAbsPath}\`）。`
  )
  lines.push('')
  lines.push('## 逐行批注')
  lines.push('')
  for (const a of p.annotations) {
    lines.push(`### \`${a.file}:${a.lineRange}\``)
    lines.push('')
    lines.push('```')
    lines.push(a.snippet)
    lines.push('```')
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
  lines.push('请按照以上批注调整代码 / 方案，完成后在终端里简述改了什么。')
  return lines.join('\n')
}
