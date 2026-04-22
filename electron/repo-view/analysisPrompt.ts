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
    '你正在做代码逻辑分析，不是代码实现。',
    '不要修改仓库文件，不要运行写操作命令，不要生成补丁。',
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
    '## 选中代码',
    '```',
    input.selection,
    '```',
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
