import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import ReactMarkdown from 'react-markdown'
import remarkGfm from 'remark-gfm'

export interface RepoSelection {
  snippet: string
  lineRange: string
}

export interface CodePaneProps {
  filePath: string
  content: string
  byteLength: number
  loading: boolean
  onAnnotateSelection: (selection: RepoSelection, comment: string, editingId?: string) => void
  editingAnnotation?: {
    id: string
    lineRange: string
    snippet: string
    comment: string
  } | null
  onCancelEditing: () => void
}

interface HighlightSegment {
  text: string
  className?: string
}

interface HighlightRange {
  start: number
  end: number
  className: string
}

const KEYWORDS = [
  'async',
  'await',
  'break',
  'case',
  'catch',
  'class',
  'const',
  'continue',
  'default',
  'else',
  'export',
  'extends',
  'finally',
  'for',
  'function',
  'if',
  'import',
  'interface',
  'implements',
  'let',
  'new',
  'private',
  'protected',
  'public',
  'return',
  'static',
  'switch',
  'throw',
  'try',
  'type',
  'var',
  'while'
] as const

const KEYWORD_SET = new Set<string>(KEYWORDS)
const KEYWORD_REGEX = new RegExp(`\\b(?:${KEYWORDS.join('|')})\\b`, 'g')
const FUNCTION_REGEX = /\b([A-Za-z_$][\w$]*)(?=\s*\()/g
const MARKDOWN_FILE_REGEX = /\.(md|markdown)$/i

function isMarkdownFile(path: string): boolean {
  return MARKDOWN_FILE_REGEX.test(path.trim())
}

function lineFromNode(node: Node | null): number | null {
  if (!node) return null
  const el = node instanceof Element ? node : node.parentElement
  const lineEl = el?.closest<HTMLElement>('.repo-code-line')
  const raw = lineEl?.dataset['line']
  if (!raw) return null
  const n = Number(raw)
  return Number.isFinite(n) ? n : null
}

function lineRangeForSelection(
  range: Range,
  root: HTMLElement
): { lo: number; hi: number } | null {
  let lo: number | null = null
  let hi: number | null = null
  const lines = root.querySelectorAll<HTMLElement>('.repo-code-line[data-line]')
  for (const ln of lines) {
    if (!range.intersectsNode(ln)) continue
    const n = Number(ln.dataset['line'])
    if (!Number.isFinite(n)) continue
    if (lo === null || n < lo) lo = n
    if (hi === null || n > hi) hi = n
  }
  if (lo === null || hi === null) return null
  return { lo, hi }
}

function parseLineRange(lineRange: string): { start: number; end: number } | null {
  const match = lineRange.match(/^(\d+)(?:-(\d+))?$/)
  if (!match) return null
  const start = Number(match[1])
  const end = Number(match[2] ?? match[1])
  if (!Number.isFinite(start) || !Number.isFinite(end)) return null
  return { start, end }
}

function collectSyntaxRanges(line: string): HighlightRange[] {
  if (!line) return []
  const ranges: HighlightRange[] = []

  KEYWORD_REGEX.lastIndex = 0
  for (let match = KEYWORD_REGEX.exec(line); match !== null; match = KEYWORD_REGEX.exec(line)) {
    ranges.push({
      start: match.index,
      end: match.index + match[0].length,
      className: 'repo-code-token-keyword'
    })
  }

  FUNCTION_REGEX.lastIndex = 0
  for (let match = FUNCTION_REGEX.exec(line); match !== null; match = FUNCTION_REGEX.exec(line)) {
    const name = match[1]
    if (!name || KEYWORD_SET.has(name)) continue
    ranges.push({
      start: match.index,
      end: match.index + name.length,
      className: 'repo-code-token-function'
    })
  }

  ranges.sort((a, b) => a.start - b.start || b.end - a.end)
  const merged: HighlightRange[] = []
  for (const range of ranges) {
    const prev = merged[merged.length - 1]
    if (!prev || range.start >= prev.end) {
      merged.push(range)
      continue
    }
    if (range.className === prev.className && range.end > prev.end) {
      prev.end = range.end
    }
  }
  return merged
}

function buildSyntaxSegments(line: string): HighlightSegment[] {
  const ranges = collectSyntaxRanges(line)
  if (ranges.length === 0) return [{ text: line || ' ' }]

  const segments: HighlightSegment[] = []
  let cursor = 0
  for (const range of ranges) {
    if (range.start > cursor) {
      segments.push({ text: line.slice(cursor, range.start) })
    }
    segments.push({
      text: line.slice(range.start, range.end),
      className: range.className
    })
    cursor = range.end
  }
  if (cursor < line.length) {
    segments.push({ text: line.slice(cursor) })
  }
  return segments
}

function applySearchHighlight(
  segments: HighlightSegment[],
  queryLower: string
): HighlightSegment[] {
  if (!queryLower) return segments

  const next: HighlightSegment[] = []
  for (const segment of segments) {
    const baseText = segment.text
    const lower = baseText.toLowerCase()
    let cursor = 0
    let idx = lower.indexOf(queryLower)

    while (idx !== -1) {
      if (idx > cursor) {
        next.push({
          text: baseText.slice(cursor, idx),
          className: segment.className
        })
      }
      const className = segment.className
        ? `${segment.className} repo-code-search-hit`
        : 'repo-code-search-hit'
      next.push({
        text: baseText.slice(idx, idx + queryLower.length),
        className
      })
      cursor = idx + queryLower.length
      idx = lower.indexOf(queryLower, cursor)
    }

    if (cursor < baseText.length) {
      next.push({
        text: baseText.slice(cursor),
        className: segment.className
      })
    }
  }

  return next.length > 0 ? next : segments
}

export default function CodePane({
  filePath,
  content,
  byteLength,
  loading,
  onAnnotateSelection,
  editingAnnotation,
  onCancelEditing
}: CodePaneProps): JSX.Element {
  const paneRef = useRef<HTMLDivElement | null>(null)
  const [draft, setDraft] = useState<{
    quote: string
    lineRange: string
    x: number
    y: number
  } | null>(null)
  const [composer, setComposer] = useState<{
    snippet: string
    lineRange: string
    comment: string
    editingId?: string
  } | null>(null)
  const [codeQuery, setCodeQuery] = useState('')
  const [activeSearchIndex, setActiveSearchIndex] = useState(0)

  const markdownMode = useMemo(() => isMarkdownFile(filePath), [filePath])
  const codeLines = useMemo(() => content.split('\n'), [content])
  const lineCount = codeLines.length
  const query = codeQuery.trim()
  const queryLower = query.toLowerCase()

  const searchMatches = useMemo(() => {
    if (!queryLower || markdownMode) return [] as Array<{ line: number }>
    const matches: Array<{ line: number }> = []
    codeLines.forEach((line, idx) => {
      const lower = line.toLowerCase()
      let cursor = 0
      while (true) {
        const at = lower.indexOf(queryLower, cursor)
        if (at < 0) break
        matches.push({ line: idx + 1 })
        cursor = at + queryLower.length
      }
    })
    return matches
  }, [codeLines, markdownMode, queryLower])

  const matchCount = searchMatches.length
  const safeActiveIndex = matchCount > 0 ? Math.min(activeSearchIndex, matchCount - 1) : 0
  const activeSearchLine = matchCount > 0 ? searchMatches[safeActiveIndex].line : null

  const linkedRange = useMemo(
    () => (editingAnnotation ? parseLineRange(editingAnnotation.lineRange) : null),
    [editingAnnotation]
  )

  useEffect(() => {
    setActiveSearchIndex(0)
  }, [queryLower, content])

  useEffect(() => {
    if (!activeSearchLine || !paneRef.current) return
    const line = paneRef.current.querySelector<HTMLElement>(
      `.repo-code-line[data-line="${activeSearchLine}"]`
    )
    line?.scrollIntoView({ block: 'center' })
  }, [activeSearchLine])

  const handleMouseUp = useCallback(() => {
    const sel = window.getSelection()
    if (!sel || sel.isCollapsed || !paneRef.current) {
      setDraft(null)
      return
    }
    const text = sel.toString().trim()
    if (!text) {
      setDraft(null)
      return
    }
    const range = sel.getRangeAt(0)
    if (!paneRef.current.contains(range.commonAncestorContainer)) {
      setDraft(null)
      return
    }
    const rects = range.getClientRects()
    const lastRect = rects.length > 0 ? rects[rects.length - 1] : range.getBoundingClientRect()
    const paneRect = paneRef.current.getBoundingClientRect()
    const span = lineRangeForSelection(range, paneRef.current)
    const fallback = lineFromNode(range.startContainer) ?? lineFromNode(range.endContainer)
    const lineRange = span
      ? span.lo === span.hi
        ? `${span.lo}`
        : `${span.lo}-${span.hi}`
      : fallback !== null
      ? `${fallback}`
      : '未知行'
    setDraft({
      quote: text,
      lineRange,
      x: lastRect.right - paneRect.left + 6,
      y: lastRect.top - paneRect.top + paneRef.current.scrollTop - 6
    })
  }, [])

  const addAnnotation = useCallback(() => {
    if (!draft) return
    setComposer({
      snippet: draft.quote,
      lineRange: draft.lineRange,
      comment: ''
    })
    setDraft(null)
  }, [draft])

  const cancelComposer = useCallback(() => {
    setComposer(null)
    onCancelEditing()
  }, [onCancelEditing])

  const saveComposer = useCallback(() => {
    if (!composer) return
    const comment = composer.comment.trim()
    if (!comment) return
    onAnnotateSelection(
      { snippet: composer.snippet, lineRange: composer.lineRange },
      comment,
      composer.editingId
    )
    setComposer(null)
    onCancelEditing()
    const sel = window.getSelection()
    sel?.removeAllRanges()
  }, [composer, onAnnotateSelection, onCancelEditing])

  const jumpSearch = useCallback(
    (offset: number) => {
      if (matchCount <= 0) return
      setActiveSearchIndex((prev) => {
        const base = ((prev + offset) % matchCount + matchCount) % matchCount
        return base
      })
    },
    [matchCount]
  )

  useEffect(() => {
    if (!editingAnnotation) return
    setComposer({
      snippet: editingAnnotation.snippet,
      lineRange: editingAnnotation.lineRange,
      comment: editingAnnotation.comment,
      editingId: editingAnnotation.id
    })
    setDraft(null)
  }, [editingAnnotation])

  return (
    <div className="repo-code-wrap">
      <div className="repo-code-head" title={filePath || '未选择文件'}>
        <span className="repo-code-head-main">
          <span className="repo-code-path">{filePath || '未选择文件'}</span>
        </span>
        <div className="repo-code-head-meta">
          {filePath && (
            <>
              <span className="repo-code-meta">{lineCount} 行</span>
              <span className="repo-code-meta">{byteLength} bytes</span>
            </>
          )}
          {!markdownMode && (
            <div className="repo-code-search-box">
              <input
                className="repo-code-search-input"
                value={codeQuery}
                onChange={(e) => setCodeQuery(e.target.value)}
                placeholder="搜索代码"
                aria-label="搜索代码"
              />
              <span className="repo-code-search-count">
                {matchCount > 0 ? `${safeActiveIndex + 1}/${matchCount}` : '0'}
              </span>
              <button
                type="button"
                className="repo-code-search-nav"
                onClick={() => jumpSearch(-1)}
                disabled={matchCount === 0}
                title="上一个"
              >
                ↑
              </button>
              <button
                type="button"
                className="repo-code-search-nav"
                onClick={() => jumpSearch(1)}
                disabled={matchCount === 0}
                title="下一个"
              >
                ↓
              </button>
            </div>
          )}
        </div>
      </div>
      <div
        className="repo-code-pane"
        ref={paneRef}
        onMouseUp={handleMouseUp}
        onScroll={() => setDraft(null)}
      >
        {!filePath ? (
          <div className="repo-code-empty">从左侧选择一个文件以查看源码</div>
        ) : loading ? (
          <div className="repo-code-empty">读取中...</div>
        ) : (
          <>
            {markdownMode ? (
              <div className="repo-code-markdown md-rendered">
                <ReactMarkdown
                  remarkPlugins={[remarkGfm]}
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
                    }
                  }}
                >
                  {content}
                </ReactMarkdown>
              </div>
            ) : (
              <pre className="repo-code-pre">
                {codeLines.map((line, index) => {
                  const lineNumber = index + 1
                  const linked =
                    linkedRange !== null &&
                    lineNumber >= linkedRange.start &&
                    lineNumber <= linkedRange.end
                  const isSearchActive = activeSearchLine === lineNumber

                  const syntaxSegments = buildSyntaxSegments(line)
                  const finalSegments = applySearchHighlight(syntaxSegments, queryLower)

                  return (
                    <div
                      key={index}
                      className={`repo-code-line${linked ? ' linked' : ''}${isSearchActive ? ' search-active' : ''}`}
                      data-line={lineNumber}
                    >
                      <span className="repo-code-gutter">{lineNumber}</span>
                      <span className="repo-code-text">
                        {finalSegments.length === 0
                          ? ' '
                          : finalSegments.map((segment, segIndex) =>
                              segment.className ? (
                                <span
                                  key={`${lineNumber}-${segIndex}`}
                                  className={segment.className}
                                >
                                  {segment.text || ' '}
                                </span>
                              ) : (
                                <span key={`${lineNumber}-${segIndex}`}>{segment.text || ' '}</span>
                              )
                            )}
                      </span>
                    </div>
                  )
                })}
              </pre>
            )}
            {draft && (
              <button
                className="repo-code-annotate-floater"
                style={{ left: draft.x, top: draft.y }}
                onClick={addAnnotation}
                title="把选中代码加入分析批注"
              >
                标注
              </button>
            )}
          </>
        )}
      </div>
      {composer && (
        <div className="plan-review-composer-backdrop" onClick={cancelComposer}>
          <div
            className="plan-review-composer"
            onClick={(e) => e.stopPropagation()}
          >
            <div className="plan-review-composer-head">
              {composer.editingId ? '编辑批注' : '添加批注'} · {filePath}:{composer.lineRange}
            </div>
            <blockquote className="plan-review-composer-quote">
              {composer.snippet.length > 2000
                ? composer.snippet.slice(0, 2000) + '\n…'
                : composer.snippet}
            </blockquote>
            <textarea
              className="plan-review-composer-input"
              value={composer.comment}
              onChange={(e) =>
                setComposer({ ...composer, comment: e.target.value })
              }
              autoFocus
              placeholder="写下这段代码需要分析或修改的具体说明..."
              rows={5}
            />
            <div className="plan-review-composer-actions">
              <button className="drawer-btn" onClick={cancelComposer}>
                取消
              </button>
              <span style={{ flex: 1 }} />
              <button
                className="drawer-btn primary"
                onClick={saveComposer}
                disabled={!composer.comment.trim()}
              >
                {composer.editingId ? '保存修改' : '添加批注'}
              </button>
            </div>
          </div>
        </div>
      )}
    </div>
  )
}
