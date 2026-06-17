export function firstMeaningfulMermaidLine(source: string): string {
  return source
    .split(/\r?\n/)
    .map((line) => line.trim())
    .find((line) => line.length > 0 && !line.startsWith('%%')) ?? ''
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
