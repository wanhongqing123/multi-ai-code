import { randomBytes } from 'crypto'
import { promises as fs } from 'fs'
import { tmpdir } from 'os'
import { join } from 'path'

export interface RuntimeAnalysisPromptFileWriteOptions {
  dir?: string
  now?: Date
  randomHex?: string
}

function timestampForFile(date: Date): string {
  return date.toISOString().replace(/[:.]/g, '-')
}

export function buildRuntimeAnalysisPromptFileMessage(filePath: string): string {
  return [
    'Please read and analyze this runtime log prompt file:',
    filePath,
    '',
    'Only analyze the issue; do not modify code. Follow the file instructions and report: Problem summary / Evidence / Likely cause / What to check first.',
  ].join('\n')
}

export async function writeRuntimeAnalysisPromptFile(
  prompt: string,
  options: RuntimeAnalysisPromptFileWriteOptions = {}
): Promise<string> {
  const dir = options.dir ?? join(tmpdir(), 'multi-ai-code', 'runtime-analysis')
  const suffix = options.randomHex ?? randomBytes(3).toString('hex')
  const filePath = join(dir, `runtime-log-${timestampForFile(options.now ?? new Date())}-${suffix}.md`)
  await fs.mkdir(dir, { recursive: true })
  await fs.writeFile(filePath, prompt, 'utf8')
  return filePath
}
