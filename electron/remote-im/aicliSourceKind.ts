import { basename } from 'path'
import type { RemoteImAicliOutputSourceKind } from './outputSanitizer.js'

function commandBaseName(command: string): string {
  const normalized = command
    .trim()
    .replace(/^["']|["']$/g, '')
    .replace(/\\/g, '/')
  return basename(normalized).toLowerCase()
}

export function getRemoteImAicliOutputSourceKind(
  command: string
): RemoteImAicliOutputSourceKind {
  const base = commandBaseName(command)
  if (/^claude(\.(exe|cmd|bat|ps1))?$/.test(base)) return 'claude'
  if (/^codex(\.(exe|cmd|bat|ps1))?$/.test(base)) return 'codex'
  if (/^opencode(\.(exe|cmd|bat|ps1))?$/.test(base)) return 'opencode'
  return 'unknown'
}
