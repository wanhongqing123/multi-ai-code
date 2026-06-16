export const BRACKETED_PASTE_START = '\x1b[200~'
export const BRACKETED_PASTE_END = '\x1b[201~'

const DEFAULT_BRACKETED_PASTE_CHUNK = 8192
const DEFAULT_SUBMIT_DELAY_MS = 80

export interface WritablePtyInput {
  write(data: string): void
}

export interface BracketedPasteChunkOptions {
  chunkSize?: number
}

export interface WriteBracketedPasteMessageOptions extends BracketedPasteChunkOptions {
  submitDelayMs?: number
  sleep?: (ms: number) => Promise<void>
}

function sanitizeBracketedPasteText(text: string): string {
  return text
    .replaceAll(BRACKETED_PASTE_START, '')
    .replaceAll(BRACKETED_PASTE_END, '')
}

export function buildBracketedPasteChunks(
  text: string,
  options: BracketedPasteChunkOptions = {}
): string[] {
  const chunkSize = Math.max(1, options.chunkSize ?? DEFAULT_BRACKETED_PASTE_CHUNK)
  const sanitized = sanitizeBracketedPasteText(text)
  const chunks = [BRACKETED_PASTE_START]
  for (let i = 0; i < sanitized.length; i += chunkSize) {
    chunks.push(sanitized.slice(i, i + chunkSize))
  }
  chunks.push(BRACKETED_PASTE_END)
  return chunks
}

export async function writeBracketedPasteMessage(
  proc: WritablePtyInput,
  text: string,
  options: WriteBracketedPasteMessageOptions = {}
): Promise<void> {
  const sleep =
    options.sleep ??
    ((ms: number) => new Promise<void>((resolve) => setTimeout(resolve, ms)))
  for (const chunk of buildBracketedPasteChunks(text, options)) {
    proc.write(chunk)
  }
  await sleep(options.submitDelayMs ?? DEFAULT_SUBMIT_DELAY_MS)
  proc.write('\r')
  await sleep(30)
  proc.write('\r')
}
