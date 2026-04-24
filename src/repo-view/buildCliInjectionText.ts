import type { RepoCodeAnnotation } from './AnalysisPanel'

const MAX_FILENAME_LEN = 200

function fnv1a8(input: string): string {
  let h = 0x811c9dc5
  for (let i = 0; i < input.length; i++) {
    h ^= input.charCodeAt(i)
    // 32-bit FNV prime multiplication, modulo 2^32
    h = Math.imul(h, 0x01000193) >>> 0
  }
  return h.toString(16).padStart(8, '0')
}

export function encodeAnalysisFileName(filePath: string): string {
  const flat = filePath.replace(/\//g, '__')
  const withExt = `${flat}.md`
  if (withExt.length <= MAX_FILENAME_LEN) return withExt
  const hash = fnv1a8(filePath)
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
  const cachePath = `.multi-ai-code/repo-view/analyses/${encodeAnalysisFileName(input.filePath)}`

  const annotationBlocks = input.annotations.map((a, i) =>
    [
      `## 标注 ${i + 1}（第 ${a.lineRange} 行）`,
      `文件: ${a.filePath}`,
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
    '## 上下文要求',
    '先自行读取该文件以及标注行号附近的完整上下文，再开始分析或修改。',
    '不要只依据这份摘要；若需要，继续向上向下扩展读取相关函数、类型和调用链。',
    '',
    '## 任务范围',
    '默认先做分析与解释，先回答这段代码在做什么、风险和边界条件。',
    '如果标注或问题明确要求修复、重构、补测试或直接落代码，可以直接修改代码，并在终端说明改了什么。',
    '',
    '## 记忆约定',
    `- 已有分析缓存：${cachePath}`,
    '- 若该文件存在，先读取并尽量复用既有结论；只补充新增内容，不重复推理',
    '- 回答完成后，把本次稳定结论以 append 形式写入该文件，记录：日期 / 行号 / 标注摘要 / 结论要点'
  ].join('\n')
}
