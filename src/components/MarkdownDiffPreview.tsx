import { Children, isValidElement, useId } from 'react'
import type { ReactElement, ReactNode } from 'react'
import ReactMarkdown from 'react-markdown'
import remarkGfm from 'remark-gfm'
import MarkdownMermaidDiagram from './MarkdownMermaidDiagram.js'
import { isMermaidCodeBlock, normalizeMixedMermaidMarkdown } from './markdownMermaid.js'

/**
 * Tiny rehype plugin that copies each element's source `position` (carried
 * over from the remark/mdast tree) into a `data-sourcepos` DOM attribute.
 *
 * Needed because react-markdown v10 removed the built-in `sourcePos` prop;
 * we still want the rendered HTML to be addressable per source line so the
 * scoped style block can highlight changed lines via attribute selectors.
 */
function rehypeAttachSourcePos() {
  return (tree: unknown) => {
    const visit = (node: unknown): void => {
      if (!node || typeof node !== 'object') return
      const n = node as {
        type?: string
        position?: { start: { line: number; column: number }; end: { line: number; column: number } }
        properties?: Record<string, unknown>
        children?: unknown[]
      }
      if (n.type === 'element' && n.position && n.properties) {
        const p = n.position
        n.properties['data-sourcepos'] = `${p.start.line}:${p.start.column}-${p.end.line}:${p.end.column}`
      }
      if (Array.isArray(n.children)) {
        for (const c of n.children) visit(c)
      }
    }
    visit(tree)
  }
}

export interface MarkdownDiffPreviewProps {
  filePath: string
  oldText: string
  newText: string
  /** 1-based line numbers in `oldText` that should render as removed. */
  oldChangedLines?: number[]
  /** 1-based line numbers in `newText` that should render as added. */
  newChangedLines?: number[]
}

type Side = 'old' | 'new'

interface CodeBlockPayload {
  code: string
  className?: string
}

function getCodeBlockPayload(children: ReactNode): CodeBlockPayload | null {
  const child = Children.toArray(children).find((item) => isValidElement(item))
  if (!isValidElement(child) || child.type !== 'code') return null

  const props = (child as ReactElement<{
    className?: string
    children?: ReactNode
  }>).props
  const rawChildren = Array.isArray(props.children)
    ? props.children.join('')
    : String(props.children ?? '')

  return {
    code: rawChildren.replace(/\n$/, ''),
    className: props.className
  }
}

/**
 * Builds the scoped CSS that highlights the changed lines for one side. We
 * use ReactMarkdown's `sourcePos` option so each rendered block element
 * receives a `data-sourcepos="<startLine>:..."` attribute; selecting on the
 * `startLine` prefix lets us light up exactly the blocks that originated
 * from a `del` / `add` source line, without needing custom AST traversal.
 */
function buildHighlightCss(rootId: string, side: Side, lines: number[]): string {
  if (lines.length === 0) return ''
  const cls = side === 'old' ? 'md-line-removed' : 'md-line-added'
  // De-duplicate to keep the rule list tight.
  const unique = Array.from(new Set(lines))
  return unique
    .map(
      (L) =>
        `#${rootId} [data-sourcepos^="${L}:"]{` +
        `background-color:var(--${cls}-bg,${
          side === 'old' ? 'rgba(239,68,68,0.16)' : 'rgba(34,197,94,0.18)'
        });` +
        `box-shadow:inset 3px 0 0 var(--${cls}-bar,${
          side === 'old' ? '#ef4444' : '#22c55e'
        });` +
        `padding-left:6px;border-radius:3px;}`
    )
    .join('\n')
}

function renderMarkdownBody(
  text: string,
  emptyLabel: string,
  side: Side,
  changedLines: number[],
  rootId: string
): JSX.Element {
  if (!text.trim()) {
    return <div className="dv-md-preview-empty">{emptyLabel}</div>
  }

  const css = buildHighlightCss(rootId, side, changedLines)
  const markdownText = normalizeMixedMermaidMarkdown(text)

  return (
    <div className={`dv-md-preview-body md-rendered md-side-${side}`} id={rootId}>
      {css && <style>{css}</style>}
      <ReactMarkdown
        remarkPlugins={[remarkGfm]}
        rehypePlugins={[rehypeAttachSourcePos]}
        components={{
          a: ({ href, children, ...rest }) => {
            const safe =
              typeof href === 'string' && /^(https?:|mailto:|#)/i.test(href)
            return safe ? (
              <a href={href} target="_blank" rel="noopener noreferrer" {...rest}>
                {children}
              </a>
            ) : (
              <span>{children}</span>
            )
          },
          img: ({ src, alt }) => {
            const safe =
              typeof src === 'string' && /^(https?:|data:image\/)/i.test(src)
            return safe ? <img src={src} alt={alt ?? ''} /> : <span>[image: {alt}]</span>
          },
          pre: ({ children, node: _node, className, ref: _ref, ...rest }) => {
            const payload = getCodeBlockPayload(children)
            if (payload && isMermaidCodeBlock(payload.code, payload.className)) {
              const wrapClassName = ['markdown-mermaid-diagram-wrap', className]
                .filter(Boolean)
                .join(' ')
              const sourcePos = (rest as Record<string, unknown>)['data-sourcepos']
              return (
                <div
                  className={wrapClassName}
                  data-sourcepos={typeof sourcePos === 'string' ? sourcePos : undefined}
                >
                  <MarkdownMermaidDiagram chart={payload.code} />
                </div>
              )
            }
            return (
              <pre className={className} {...rest}>
                {children}
              </pre>
            )
          }
        }}
      >
        {markdownText}
      </ReactMarkdown>
    </div>
  )
}

export default function MarkdownDiffPreview({
  filePath,
  oldText,
  newText,
  oldChangedLines = [],
  newChangedLines = []
}: MarkdownDiffPreviewProps): JSX.Element {
  const reactId = useId()
  // useId returns ":r0:"-style strings that aren't valid CSS id selectors
  // unmodified; strip the colons so we can use it in a `#…` selector.
  const safeId = reactId.replace(/:/g, '')
  const oldRootId = `md-old-${safeId}`
  const newRootId = `md-new-${safeId}`

  const changedSummary = (() => {
    const added = newChangedLines.length
    const removed = oldChangedLines.length
    if (added === 0 && removed === 0) return null
    const parts: string[] = []
    if (removed > 0) parts.push(`-${removed}`)
    if (added > 0) parts.push(`+${added}`)
    return parts.join(' ')
  })()

  return (
    <section className="dv-md-preview" aria-label={`Markdown Preview: ${filePath}`}>
      <div className="dv-md-preview-title">
        Markdown Preview
        {changedSummary && (
          <span className="dv-md-preview-badge">{changedSummary}</span>
        )}
      </div>
      <div className="dv-md-preview-grid">
        <section className="dv-md-preview-col">
          {renderMarkdownBody(
            oldText,
            'No previous content',
            'old',
            oldChangedLines,
            oldRootId
          )}
        </section>
        <section className="dv-md-preview-col">
          {renderMarkdownBody(
            newText,
            'No current content',
            'new',
            newChangedLines,
            newRootId
          )}
        </section>
      </div>
    </section>
  )
}
