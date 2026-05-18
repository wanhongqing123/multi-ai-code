const MARKDOWN_FILE_RE = /\.(md|markdown)$/i

export interface MarkdownPreviewDiffLine {
  kind: 'add' | 'del' | 'context' | 'hunk' | 'meta'
  text: string
}

export function isMarkdownDiffPath(path: string): boolean {
  return MARKDOWN_FILE_RE.test(path.trim())
}

export interface MarkdownPreviewResult {
  oldText: string
  newText: string
  /** 1-based line numbers in `oldText` that came from `del` lines. */
  oldChangedLines: number[]
  /** 1-based line numbers in `newText` that came from `add` lines. */
  newChangedLines: number[]
}

export function buildMarkdownPreviewText(
  lines: MarkdownPreviewDiffLine[]
): MarkdownPreviewResult {
  const oldParts: string[] = []
  const newParts: string[] = []
  const oldChangedLines: number[] = []
  const newChangedLines: number[] = []
  let oldLine = 0
  let newLine = 0

  for (const line of lines) {
    if (line.kind === 'context') {
      oldParts.push(line.text)
      newParts.push(line.text)
      oldLine++
      newLine++
      continue
    }
    if (line.kind === 'del') {
      oldParts.push(line.text)
      oldLine++
      oldChangedLines.push(oldLine)
      continue
    }
    if (line.kind === 'add') {
      newParts.push(line.text)
      newLine++
      newChangedLines.push(newLine)
    }
  }

  return {
    oldText: oldParts.join('\n'),
    newText: newParts.join('\n'),
    oldChangedLines,
    newChangedLines
  }
}
