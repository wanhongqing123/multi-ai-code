export interface SessionAnnotation {
  /** Relative path of the annotated file (from target_repo root). */
  file: string
  /** "10" or "10-12" - line number or inclusive range. */
  lineRange: string
  /** The exact code snippet the user highlighted. */
  snippet: string
  /** User's comment on this location. */
  comment: string
}

export interface AnnotationsForSessionParams {
  annotations: SessionAnnotation[]
  /** User's optional overall comment on the whole diff. */
  generalComment: string
}

export function formatAnnotationsForSession(
  p: AnnotationsForSessionParams
): string {
  const lines: string[] = []
  lines.push('# 用户批注')
  lines.push('')
  lines.push('以下是用户对当前改动的批注，请严格按照批注执行，直接修改代码。')
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
  lines.push('请按照以上批注调整代码，完成后在终端里简述改了什么。')
  return lines.join('\n')
}
