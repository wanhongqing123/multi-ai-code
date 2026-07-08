export type TerminalStyleCli = 'claude' | 'codex' | 'opencode' | 'unknown'

const SGR_PATTERN = /\x1b\[([0-9;]*)m/g

export interface TerminalStyleOptions {
  platform?: string
}

export function normalizeTerminalStyleForCli(
  chunk: string,
  cli: TerminalStyleCli,
  options: TerminalStyleOptions = {}
): string {
  if (cli !== 'codex' || chunk.length === 0 || !isWindowsPlatform(options.platform)) {
    return chunk
  }

  return chunk.replace(SGR_PATTERN, (sequence, params: string) => {
    const stripped = stripCodexBackgroundSgrParams(params)
    if (stripped === null) return sequence
    if (stripped.length === 0) return ''
    return `\x1b[${stripped.join(';')}m`
  })
}

function isWindowsPlatform(platform = currentPlatform()): boolean {
  return platform.toLowerCase().includes('win')
}

function currentPlatform(): string {
  return typeof navigator !== 'undefined' ? navigator.platform : ''
}

function stripCodexBackgroundSgrParams(params: string): string[] | null {
  if (params.length === 0) return null

  const parts = params.split(';')
  const kept: string[] = []
  let changed = false

  for (let i = 0; i < parts.length; ) {
    const value = Number(parts[i])

    if (Number.isInteger(value)) {
      if (value === 48) {
        changed = true
        i += extendedColorParamLength(parts, i)
        continue
      }

      if (value === 7 || isBasicBackgroundColor(value)) {
        changed = true
        i += 1
        continue
      }
    }

    kept.push(parts[i])
    i += 1
  }

  return changed ? kept : null
}

function isBasicBackgroundColor(value: number): boolean {
  return (value >= 40 && value <= 49) || (value >= 100 && value <= 107)
}

function extendedColorParamLength(parts: string[], index: number): number {
  const mode = Number(parts[index + 1])
  if (mode === 5 && index + 2 < parts.length) return 3
  if (mode === 2 && index + 4 < parts.length) return 5
  return 1
}
