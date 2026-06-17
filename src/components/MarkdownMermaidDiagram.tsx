import { useEffect, useId, useMemo, useState } from 'react'
import { getRenderableMermaidChart } from './markdownMermaid.js'

function hashSource(source: string): string {
  let hash = 0
  for (let i = 0; i < source.length; i += 1) {
    hash = Math.imul(31, hash) + source.charCodeAt(i)
    hash |= 0
  }
  return Math.abs(hash).toString(36)
}

interface MermaidParseErrorDetails {
  hash?: {
    loc?: {
      first_line?: number
      first_column?: number
    }
    token?: string
  }
}

function isMermaidParseErrorDetails(error: unknown): error is MermaidParseErrorDetails {
  return typeof error === 'object' && error !== null && 'hash' in error
}

export function formatMermaidErrorMessage(error: unknown): string {
  const message = error instanceof Error ? error.message : String(error)
  if (!isMermaidParseErrorDetails(error)) return message

  const details: string[] = []
  const loc = error.hash?.loc
  if (typeof loc?.first_line === 'number') {
    details.push(`line ${loc.first_line}`)
  }
  if (typeof loc?.first_column === 'number') {
    details.push(`column ${loc.first_column}`)
  }
  if (error.hash?.token) {
    details.push(`token ${error.hash.token}`)
  }

  return details.length > 0 ? `${message} (${details.join(', ')})` : message
}

export interface MarkdownMermaidDiagramProps {
  chart: string
}

export default function MarkdownMermaidDiagram({
  chart
}: MarkdownMermaidDiagramProps): JSX.Element {
  const reactId = useId()
  const renderChart = useMemo(() => getRenderableMermaidChart(chart), [chart])
  const diagramId = useMemo(
    () => `md-mermaid-${reactId.replace(/:/g, '')}-${hashSource(renderChart)}`,
    [renderChart, reactId]
  )
  const [svg, setSvg] = useState<string | null>(null)
  const [error, setError] = useState<string | null>(null)

  useEffect(() => {
    let cancelled = false

    async function renderDiagram(): Promise<void> {
      setSvg(null)
      setError(null)

      try {
        const mermaidModule = await import('mermaid')
        const mermaid = mermaidModule.default
        const dark = document.documentElement.classList.contains('theme-dark')
        mermaid.initialize({
          startOnLoad: false,
          securityLevel: 'strict',
          suppressErrorRendering: true,
          theme: dark ? 'dark' : 'default',
          fontFamily: 'Inter, Noto Sans SC, sans-serif'
        })
        await mermaid.parse(renderChart)
        const result = await mermaid.render(diagramId, renderChart)
        if (!cancelled) {
          setSvg(result.svg)
        }
      } catch (err) {
        if (!cancelled) {
          setError(formatMermaidErrorMessage(err))
        }
      }
    }

    void renderDiagram()

    return () => {
      cancelled = true
    }
  }, [renderChart, diagramId])

  return (
    <div className="markdown-mermaid-diagram" data-diagram-id={diagramId}>
      {svg ? (
        <div
          className="markdown-mermaid-svg"
          dangerouslySetInnerHTML={{ __html: svg }}
        />
      ) : error ? (
        <div className="markdown-mermaid-error" role="alert">
          Mermaid render failed: {error}
        </div>
      ) : (
        <div className="markdown-mermaid-loading" aria-live="polite">
          Rendering diagram...
        </div>
      )}
    </div>
  )
}
