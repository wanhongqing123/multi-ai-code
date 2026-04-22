export function buildRepoAnalysisPrompt(input: {
  repoRoot: string
  filePath: string
  selection: string
  question: string
  projectSummary: string
  fileNote: string
}): string {
  return [
    '# 角色',
    '',
    '你正在处理仓库代码片段的审查、分析或修改请求。',
    '先根据片段说明理解意图。',
    '如果用户明确要求修改代码、补测试或调整实现，可以直接在当前仓库完成。',
    '如果用户没有明确要求修改，优先输出分析结论、风险和建议。',
    '',
    '# 仓库上下文',
    '',
    `仓库根：${input.repoRoot}`,
    `文件：${input.filePath}`,
    '',
    '## 项目记忆',
    input.projectSummary || '（暂无项目记忆）',
    '',
    '## 文件记忆',
    input.fileNote || '（暂无文件记忆）',
    '',
    '## 用户提供的代码片段与说明',
    input.selection,
    '',
    '## 用户问题',
    input.question || '请说明这段代码的主流程、关键约束和潜在风险。',
    '',
    '# 输出格式',
    '',
    '先输出给用户看的分析内容。',
    '结尾必须追加：',
    '[[MEMORY_UPDATE]]',
    '1. 本次稳定结论',
    '2. 值得并入项目记忆的事实',
    '[[END_OF_ANALYSIS]]'
  ].join('\n')
}
