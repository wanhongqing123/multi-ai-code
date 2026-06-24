export interface OutputChunkOptions {
  maxChunkChars: number
}

export function stripTerminalControl(input: string): string {
  return input
    .replace(/\u001b\[[0-?]*[ -/]*[@-~]/g, '')
    .replace(/\u001b\][^\u0007]*(\u0007|\u001b\\)/g, '')
    .replace(/\r\n/g, '\n')
    .replace(/\r/g, '\n')
    .replace(/\u001b[=>]/g, '')
    .trim()
}

export function createOutputChunks(input: string, options: OutputChunkOptions): string[] {
  const clean = stripTerminalControl(input)
  if (!clean) return []
  const maxChunkChars = Math.max(1, Math.round(options.maxChunkChars))
  const chunks: string[] = []
  for (let index = 0; index < clean.length; index += maxChunkChars) {
    chunks.push(clean.slice(index, index + maxChunkChars))
  }
  return chunks
}
