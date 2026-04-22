const MEMORY_TAG = '[[MEMORY_UPDATE]]'
const END_TAG = '[[END_OF_ANALYSIS]]'

export function parseAnalysisOutput(raw: string): {
  answer: string
  memoryUpdate: string
  complete: boolean
} {
  const memoryIdx = raw.indexOf(MEMORY_TAG)
  const endIdx = raw.indexOf(END_TAG)
  const complete = endIdx >= 0
  if (memoryIdx < 0) {
    return {
      answer: complete ? raw.slice(0, endIdx).trim() : raw,
      memoryUpdate: '',
      complete
    }
  }
  const answer = raw.slice(0, memoryIdx).trim()
  const memoryRaw = complete
    ? raw.slice(memoryIdx + MEMORY_TAG.length, endIdx)
    : raw.slice(memoryIdx + MEMORY_TAG.length)
  return {
    answer,
    memoryUpdate: memoryRaw.trim(),
    complete
  }
}
