import { join } from 'path'

export function planSystemPromptInjection(input: {
  command: 'claude' | 'codex'
  cwd: string
  systemPrompt: string
  initialUserMessage: string
}): {
  writeDir: string
  writePath: string
  fileContents: string
  bootstrapMessage: string
} {
  const writeDir = join(input.cwd, '.injections')
  const filename =
    input.command === 'claude' ? 'claude-system.md' : 'codex-system.md'
  const writePath = join(writeDir, filename)
  const bootstrapMessage = [
    `Please fully read ${writePath} as the system role and constraints for this task before doing any work.`,
    '',
    input.initialUserMessage
  ].join('\n')

  return {
    writeDir,
    writePath,
    fileContents: input.systemPrompt,
    bootstrapMessage
  }
}
