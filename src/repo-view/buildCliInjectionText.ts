import type { RepoCodeAnnotation } from './AnalysisPanel'

export interface BuildCliInjectionTextInput {
  repoRoot: string
  annotations: RepoCodeAnnotation[]
  question: string
}

export function buildCliInjectionText(
  input: BuildCliInjectionTextInput
): string {
  const filePaths = Array.from(new Set(input.annotations.map((annotation) => annotation.filePath)))
  const primaryFilePath = filePaths[0] ?? 'unknown-file'

  const annotationBlocks = input.annotations.map((annotation, index) =>
    [
      `## 标注 ${index + 1}（${annotation.filePath} 第 ${annotation.lineRange} 行）`,
      `文件: ${annotation.filePath}`,
      '代码片段：',
      '```',
      annotation.snippet,
      '```',
      `说明: ${annotation.comment}`
    ].join('\n')
  )

  const question = input.question.trim() || '请按标注分析'
  const fileSummary = filePaths.length === 1
    ? [`文件: ${primaryFilePath}`]
    : [
        `文件数: ${filePaths.length}`,
        '文件列表:',
        ...filePaths.map((filePath) => `- ${filePath}`)
      ]

  return [
    `仓库根: ${input.repoRoot}`,
    ...fileSummary,
    '',
    annotationBlocks.join('\n\n'),
    '',
    '## 问题',
    question,
    '',
    '## 上下文要求',
    '先自行读取相关文件以及标注行号附近的完整上下文，再开始分析或修改。',
    '不要只依赖这份摘要；若需要，继续向上向下扩展读取相关函数、类型和调用链。',
    '',
    '## 任务范围',
    '默认先做分析与解释，先回答这些代码在做什么、风险和边界条件。',
    '如果标注或问题明确要求修复、重构、补测试或直接落代码，可以直接修改代码，并在终端说明改了什么。'
  ].join('\n')
}
