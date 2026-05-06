const MARKDOWN_FILE_RE = /\.(md|markdown)$/i

export interface MarkdownPreviewDiffLine {
  kind: 'add' | 'del' | 'context' | 'hunk' | 'meta'
  text: string
}

export function isMarkdownDiffPath(path: string): boolean {
  return MARKDOWN_FILE_RE.test(path.trim())
}

export function buildMarkdownPreviewText(
  lines: MarkdownPreviewDiffLine[]
): {
  oldText: string
  newText: string
} {
  const oldParts: string[] = []
  const newParts: string[] = []

  for (const line of lines) {
    if (line.kind === 'context') {
      oldParts.push(line.text)
      newParts.push(line.text)
      continue
    }
    if (line.kind === 'del') {
      oldParts.push(line.text)
      continue
    }
    if (line.kind === 'add') {
      newParts.push(line.text)
    }
  }

  return {
    oldText: oldParts.join('\n'),
    newText: newParts.join('\n')
  }
}
