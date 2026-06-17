export function firstMeaningfulMermaidLine(source: string): string {
  return source
    .split(/\r?\n/)
    .map((line) => line.trim())
    .find((line) => line.length > 0 && !line.startsWith('%%')) ?? ''
}

function splitLines(source: string): string[] {
  return source.split(/\r?\n/)
}

export function isMermaidCodeBlock(source: string, className?: string): boolean {
  const hasMermaidClass = /\blanguage-mermaid\b|\bmermaid\b/.test(className ?? '')
  const firstLine = firstMeaningfulMermaidLine(source)
  return hasMermaidClass || /^sequenceDiagram\b/.test(firstLine)
}

export function isMermaidSequenceDiagram(source: string, className?: string): boolean {
  if (!isMermaidCodeBlock(source, className)) return false
  return /^sequenceDiagram\b/.test(firstMeaningfulMermaidLine(source))
}

function isSequenceMessageLine(line: string): boolean {
  return /^\S+(?:\s*,\s*\S+)*\s*(?:-->>|->>|-->|->|--x|->x|--\)|-\)|--\+|-\+)\s*\S+(?:\s*,\s*\S+)*\s*:/.test(
    line.trim()
  )
}

function isSequenceDiagramSyntaxLine(line: string): boolean {
  const trimmed = line.trim()
  if (!trimmed) return true
  if (trimmed.startsWith('%%')) return true
  if (/^sequenceDiagram\b/.test(trimmed)) return true
  if (/^autonumber\b/.test(trimmed)) return true
  if (/^(participant|actor)\s+\S+/.test(trimmed)) return true
  if (/^create\s+(participant|actor)\s+\S+/.test(trimmed)) return true
  if (/^(activate|deactivate|destroy)\s+\S+/.test(trimmed)) return true
  if (/^Note\s+(?:over|right of|left of)\s+.+:/.test(trimmed)) return true
  if (/^(loop|alt|else|opt|par|and|critical|option|break|rect)\b/.test(trimmed)) {
    return true
  }
  if (/^box\b/.test(trimmed)) return true
  if (/^end\b/.test(trimmed)) return true
  if (/^(link|links|properties)\s+\S+/.test(trimmed)) return true
  return isSequenceMessageLine(trimmed)
}

export interface SequenceDiagramSplit {
  diagram: string
  trailing: string
  diagramLineCount: number
}

export function splitSequenceDiagramMixedContent(source: string): SequenceDiagramSplit {
  const lines = splitLines(source)
  const firstDiagramLine = lines.findIndex((line) => /^sequenceDiagram\b/.test(line.trim()))
  if (firstDiagramLine === -1) {
    return { diagram: source, trailing: '', diagramLineCount: lines.length }
  }

  let cursor = firstDiagramLine
  while (cursor < lines.length && isSequenceDiagramSyntaxLine(lines[cursor])) {
    cursor += 1
  }

  let diagramEnd = cursor
  while (diagramEnd > firstDiagramLine && !lines[diagramEnd - 1].trim()) {
    diagramEnd -= 1
  }

  return {
    diagram: lines.slice(0, diagramEnd).join('\n'),
    trailing: lines.slice(diagramEnd).join('\n'),
    diagramLineCount: diagramEnd
  }
}

export function getRenderableMermaidChart(source: string): string {
  if (!isMermaidSequenceDiagram(source, 'language-mermaid')) return source
  return splitSequenceDiagramMixedContent(source).diagram
}

interface MarkdownFence {
  marker: string
  info: string
}

function parseFenceStart(line: string): MarkdownFence | null {
  const match = line.match(/^\s{0,3}(`{3,}|~{3,})(.*)$/)
  if (!match) return null
  const marker = match[1]
  if (!/^`+$/.test(marker) && !/^~+$/.test(marker)) return null
  return { marker, info: match[2].trim() }
}

function isFenceEnd(line: string, fence: MarkdownFence): boolean {
  const markerChar = fence.marker[0]
  const escaped = markerChar === '`' ? '`' : '~'
  const re = new RegExp(`^\\s{0,3}${escaped}{${fence.marker.length},}\\s*$`)
  return re.test(line)
}

function isMermaidFence(fence: MarkdownFence): boolean {
  return /\bmermaid\b/i.test(fence.info)
}

export function normalizeMixedMermaidMarkdown(markdown: string): string {
  const lines = splitLines(markdown)
  const out: string[] = []
  let i = 0

  while (i < lines.length) {
    const fence = parseFenceStart(lines[i])
    if (fence) {
      const fenceLine = lines[i]
      const codeStart = i + 1
      let codeEnd = codeStart
      while (codeEnd < lines.length && !isFenceEnd(lines[codeEnd], fence)) {
        codeEnd += 1
      }
      const hasFenceEnd = codeEnd < lines.length
      const code = lines.slice(codeStart, codeEnd).join('\n')

      if (isMermaidFence(fence) && isMermaidSequenceDiagram(code, 'language-mermaid')) {
        const split = splitSequenceDiagramMixedContent(code)
        out.push(fenceLine)
        out.push(...split.diagram.split('\n'))
        out.push(fence.marker)
        if (split.trailing) {
          out.push(...split.trailing.split('\n'))
        }
      } else {
        out.push(fenceLine)
        out.push(...lines.slice(codeStart, codeEnd))
        if (hasFenceEnd) out.push(lines[codeEnd])
      }

      i = hasFenceEnd ? codeEnd + 1 : codeEnd
      continue
    }

    if (/^sequenceDiagram\b/.test(lines[i].trim())) {
      const rest = lines.slice(i).join('\n')
      const split = splitSequenceDiagramMixedContent(rest)
      out.push('```mermaid')
      out.push(...split.diagram.split('\n'))
      out.push('```')
      i += split.diagramLineCount
      continue
    }

    out.push(lines[i])
    i += 1
  }

  return out.join('\n')
}
