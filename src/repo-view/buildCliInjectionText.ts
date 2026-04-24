import { createHash } from 'crypto'
import type { RepoCodeAnnotation } from './AnalysisPanel'

const MAX_FILENAME_LEN = 200

const EXT_TO_LANG: Record<string, string> = {
  c: 'c',
  h: 'c',
  cc: 'cpp',
  cpp: 'cpp',
  hpp: 'cpp',
  cs: 'csharp',
  go: 'go',
  java: 'java',
  js: 'js',
  jsx: 'jsx',
  json: 'json',
  kt: 'kotlin',
  m: 'objc',
  mm: 'objc',
  md: 'markdown',
  py: 'python',
  rb: 'ruby',
  rs: 'rust',
  sh: 'bash',
  swift: 'swift',
  toml: 'toml',
  ts: 'ts',
  tsx: 'tsx',
  yaml: 'yaml',
  yml: 'yaml'
}

function langForFile(filePath: string): string {
  const dot = filePath.lastIndexOf('.')
  if (dot < 0) return ''
  return EXT_TO_LANG[filePath.slice(dot + 1).toLowerCase()] ?? ''
}

export function encodeAnalysisFileName(filePath: string): string {
  const flat = filePath.replace(/\//g, '__')
  const withExt = `${flat}.md`
  if (withExt.length <= MAX_FILENAME_LEN) return withExt
  const hash = createHash('sha1').update(filePath).digest('hex').slice(0, 8)
  // budget: head + '__' + hash + '.md' ≤ MAX_FILENAME_LEN
  const head = flat.slice(0, MAX_FILENAME_LEN - (2 + 8 + 3))
  return `${head}__${hash}.md`
}

export interface BuildCliInjectionTextInput {
  repoRoot: string
  filePath: string
  annotations: RepoCodeAnnotation[]
  question: string
}

export function buildCliInjectionText(
  input: BuildCliInjectionTextInput
): string {
  const lang = langForFile(input.filePath)
  const fenceOpen = lang ? '```' + lang : '```'
  const cachePath = `.multi-ai-code/repo-view/analyses/${encodeAnalysisFileName(input.filePath)}`

  const annotationBlocks = input.annotations.map((a, i) =>
    [
      `## 标注 ${i + 1}（第 ${a.lineRange} 行）`,
      fenceOpen,
      a.snippet,
      '```',
      `说明: ${a.comment}`
    ].join('\n')
  )

  const question = input.question.trim() || '请按标注分析'

  return [
    `仓库根: ${input.repoRoot}`,
    `文件: ${input.filePath}`,
    '',
    annotationBlocks.join('\n\n'),
    '',
    '## 问题',
    question,
    '',
    '## 记忆约定',
    `- 已有分析缓存：${cachePath}`,
    '- 若该文件存在，先读取并尽量复用既有结论；只补充新增内容，不重复推理',
    '- 回答完成后，把本次稳定结论以 append 形式写入该文件，记录：日期 / 行号 / 标注摘要 / 结论要点'
  ].join('\n')
}
