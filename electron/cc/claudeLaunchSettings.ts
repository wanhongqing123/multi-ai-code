import { basename } from 'path'

export const EMBEDDED_CLAUDE_SETTINGS = {
  tui: 'default'
} as const

function isClaudeCommand(command: string): boolean {
  const normalized = command
    .trim()
    .replace(/^["']|["']$/g, '')
    .replace(/\\/g, '/')
  const base = basename(normalized).toLowerCase()
  return /^claude(\.(exe|cmd|bat|ps1))?$/.test(base)
}

export function withEmbeddedClaudeSettings(
  command: string,
  args: string[]
): string[] {
  if (!isClaudeCommand(command)) return args
  return [...args, '--settings', JSON.stringify(EMBEDDED_CLAUDE_SETTINGS)]
}
