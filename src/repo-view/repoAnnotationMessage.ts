interface RepoAnnotationMessageItem {
  id: string
  filePath: string
  lineRange: string
  snippet: string
  comment: string
}

export function buildRepoAnnotationMessage(input: {
  filePath: string
  question: string
  annotations: RepoAnnotationMessageItem[]
}): string {
  const lines: string[] = [
    `文件：${input.filePath}`,
    '',
    '以下是本次选中的代码片段和对应说明，请严格按这些要求处理。'
  ]

  input.annotations.forEach((annotation, index) => {
    lines.push(
      '',
      `## 片段 ${index + 1}（行 ${annotation.lineRange}）`,
      '',
      '代码：',
      ...annotation.snippet.split('\n').map((line) => `> ${line}`),
      '',
      '说明：',
      annotation.comment
    )
  })

  if (input.question.trim()) {
    lines.push('', '## 整体要求', '', input.question.trim())
  } else {
    lines.push('', '## 整体要求', '', '请先解释这些片段的逻辑和风险；如果片段说明明确要求修改代码，就直接修改并说明改动。')
  }

  return lines.join('\n')
}
