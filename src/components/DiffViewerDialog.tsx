import {
  type CSSProperties,
  type Dispatch,
  type SetStateAction,
  useCallback,
  useDeferredValue,
  useEffect,
  useMemo,
  useRef,
  useState
} from 'react'
import { placeDraftButton } from './diffAnnotationPosition.js'
import { applyControlledStateUpdate } from './controlledState.js'
import {
  DIFF_MODE_TABS,
  diffModeLabel,
  type DiffMode
} from './diffViewerConfig.js'
import { getHorizontalTrackpadDelta } from './diffViewerScroll.js'

export interface DiffAnnotation {
  id: string
  file: string
  lineRange: string
  snippet: string
  comment: string
}

export interface DiffViewerDialogProps {
  cwd: string
  title?: string
  onClose: () => void
  onSubmit: (annotations: DiffAnnotation[], generalNote: string) => Promise<void> | void
  /** Whether the live AI session is running — gates the send button. */
  sessionRunning?: boolean
  /** Controlled annotations list — lifted to parent so unsent batches survive
   *  a dialog close/reopen. Parent clears it after a successful submit. */
  annotations: DiffAnnotation[]
  onAnnotationsChange: Dispatch<SetStateAction<DiffAnnotation[]>>
  /** Controlled general-note field — same persistence rationale as annotations. */
  generalNote: string
  onGeneralNoteChange: Dispatch<SetStateAction<string>>
  /** Controlled diff-mode tab + commit + file selection. Lifted so that closing
   *  and reopening the dialog against the same repo keeps the tab and the
   *  selected file. Parent resets these on project switch. */
  mode: DiffMode
  onModeChange: Dispatch<SetStateAction<DiffMode>>
  selectedCommit: string
  onSelectedCommitChange: Dispatch<SetStateAction<string>>
  selectedFile: string
  onSelectedFileChange: Dispatch<SetStateAction<string>>
}

interface CommitEntry {
  hash: string
  short: string
  author: string
  date: string
  subject: string
}

interface DiffLine {
  kind: 'add' | 'del' | 'context' | 'hunk' | 'meta'
  text: string
  oldLine?: number
  newLine?: number
}

interface DiffFile {
  path: string
  header: string[]
  lines: DiffLine[]
}

type PairedRow =
  | { kind: 'pair'; left?: DiffLine; right?: DiffLine }
  | { kind: 'hunk'; text: string }

interface SearchHit {
  rowIndex: number
  side: 'left' | 'right'
  rangeIndex: number
}

interface DiffFileTreeNode {
  name: string
  fullPath: string
  type: 'file' | 'dir'
  children?: DiffFileTreeNode[]
}

function findMatchRanges(text: string, query: string): Array<[number, number]> {
  if (!query) return []
  const source = text.toLocaleLowerCase()
  const needle = query.toLocaleLowerCase()
  if (!needle) return []
  const out: Array<[number, number]> = []
  let from = 0
  while (from <= source.length - needle.length) {
    const idx = source.indexOf(needle, from)
    if (idx < 0) break
    out.push([idx, idx + needle.length])
    from = idx + needle.length
  }
  return out
}

function rowMatchesQuery(row: PairedRow, query: string): boolean {
  if (!query || row.kind !== 'pair') return false
  const q = query.toLocaleLowerCase()
  return (
    (row.left?.text.toLocaleLowerCase().includes(q) ?? false) ||
    (row.right?.text.toLocaleLowerCase().includes(q) ?? false)
  )
}

function renderHighlightedText(
  text: string,
  query: string,
  activeRangeIndex: number | null
): JSX.Element | string {
  if (!query) return text
  const ranges = findMatchRanges(text, query)
  if (ranges.length === 0) return text
  const parts: JSX.Element[] = []
  let cursor = 0
  for (let i = 0; i < ranges.length; i++) {
    const [start, end] = ranges[i]
    if (start > cursor) {
      parts.push(
        <span key={`txt_${i}_${start}`}>{text.slice(cursor, start)}</span>
      )
    }
    parts.push(
      <mark
        key={`mark_${i}_${start}`}
        className={
          activeRangeIndex === i
            ? 'dv-search-mark dv-search-mark-active'
            : 'dv-search-mark'
        }
      >
        {text.slice(start, end)}
      </mark>
    )
    cursor = end
  }
  if (cursor < text.length) {
    parts.push(<span key={`tail_${cursor}`}>{text.slice(cursor)}</span>)
  }
  return <>{parts}</>
}

/** Fold a unified-diff line stream into side-by-side pairs. Consecutive
 *  del+add runs are zipped (so a modify shows the removed code on the left
 *  next to the added code on the right); pure deletes / inserts become rows
 *  with only one side populated. Hunk headers span both columns. */
function pairLines(lines: DiffLine[]): PairedRow[] {
  const out: PairedRow[] = []
  const dels: DiffLine[] = []
  const adds: DiffLine[] = []
  const flush = (): void => {
    const n = Math.max(dels.length, adds.length)
    for (let i = 0; i < n; i++) {
      out.push({ kind: 'pair', left: dels[i], right: adds[i] })
    }
    dels.length = 0
    adds.length = 0
  }
  for (const ln of lines) {
    if (ln.kind === 'del') {
      dels.push(ln)
    } else if (ln.kind === 'add') {
      adds.push(ln)
    } else if (ln.kind === 'hunk') {
      flush()
      out.push({ kind: 'hunk', text: ln.text })
    } else if (ln.kind === 'context') {
      flush()
      out.push({ kind: 'pair', left: ln, right: ln })
    }
  }
  flush()
  return out
}

function genId(): string {
  return `ann_${Date.now().toString(36)}_${Math.random().toString(36).slice(2, 8)}`
}

/** Estimated height of one rendered `.dv-row`. Used by the row virtualizer
 *  to compute spacer heights and by jumpHunk to scroll to a target index.
 *  Line-height 1.6 × 14px + 2×2px padding ≈ 26px; we underestimate slightly
 *  so overscan (+40 rows) compensates on fast scrolls. */
const DV_ROW_H = 24

/** Render only rows in the current scroll viewport of `paneRef`, plus
 *  overscan. Spacer divs above/below preserve total scroll height so the
 *  scrollbar behaves normally. */
function VirtualizedFileRows({
  filePath,
  rows,
  paneRef,
  searchQuery,
  activeSearchHit,
  annotationsByLine,
  onEditAnnotation,
  collapsedAnnotationIds,
  onToggleAnnotation,
  onDeleteAnnotation
}: {
  filePath: string
  rows: PairedRow[]
  paneRef: React.RefObject<HTMLDivElement | null>
  searchQuery: string
  activeSearchHit: SearchHit | null
  annotationsByLine: Map<number, DiffAnnotation[]>
  onEditAnnotation: (a: DiffAnnotation) => void
  collapsedAnnotationIds: Set<string>
  onToggleAnnotation: (id: string) => void
  onDeleteAnnotation: (id: string) => void
}): JSX.Element {
  const containerRef = useRef<HTMLDivElement>(null)
  const [range, setRange] = useState<[number, number]>(() => [
    0,
    Math.min(rows.length, 200)
  ])

  // Reset to top when the file (rows) changes.
  useEffect(() => {
    setRange([0, Math.min(rows.length, 200)])
  }, [rows])

  useEffect(() => {
    const pane = paneRef.current
    const container = containerRef.current
    if (!pane || !container) return
    let raf = 0
    const compute = (): void => {
      const paneRect = pane.getBoundingClientRect()
      const ctRect = container.getBoundingClientRect()
      // How many px from the top of `container` to the top of pane's visible
      // region (can be negative if container starts below the viewport).
      const scrolledPastContainer = paneRect.top - ctRect.top
      const viewTop = Math.max(0, scrolledPastContainer)
      const viewBottom = viewTop + pane.clientHeight
      const OVER = 40
      const start = Math.max(0, Math.floor(viewTop / DV_ROW_H) - OVER)
      const end = Math.min(
        rows.length,
        Math.ceil(viewBottom / DV_ROW_H) + OVER
      )
      setRange((prev) =>
        prev[0] === start && prev[1] === end ? prev : [start, end]
      )
    }
    const schedule = (): void => {
      if (raf) return
      raf = requestAnimationFrame(() => {
        raf = 0
        compute()
      })
    }
    compute()
    pane.addEventListener('scroll', schedule, { passive: true })
    const ro = new ResizeObserver(schedule)
    ro.observe(pane)
    return () => {
      pane.removeEventListener('scroll', schedule)
      ro.disconnect()
      if (raf) cancelAnimationFrame(raf)
    }
  }, [rows.length, paneRef])

  const [start, end] = range
  const topPad = start * DV_ROW_H
  const bottomPad = Math.max(0, (rows.length - end) * DV_ROW_H)

  const isChangeRow = (r: PairedRow): boolean =>
    r.kind === 'pair' && (r.left?.kind === 'del' || r.right?.kind === 'add')

  // Per-side max content width (in mono characters). Drives two fake
  // horizontal scrollbars below; each side's scroll translates the code
  // content inside its cells independently.
  const { maxLeftChars, maxRightChars } = useMemo(() => {
    let ml = 0
    let mr = 0
    for (const r of rows) {
      if (r.kind === 'hunk') {
        if (r.text.length > ml) ml = r.text.length
        if (r.text.length > mr) mr = r.text.length
      } else {
        const l = r.left?.text.length ?? 0
        const rt = r.right?.text.length ?? 0
        if (l > ml) ml = l
        if (rt > mr) mr = rt
      }
    }
    return { maxLeftChars: ml, maxRightChars: mr }
  }, [rows])

  const leftScrollRef = useRef<HTMLDivElement>(null)
  const rightScrollRef = useRef<HTMLDivElement>(null)

  // Reset scroll position when switching files.
  useEffect(() => {
    if (leftScrollRef.current) leftScrollRef.current.scrollLeft = 0
    if (rightScrollRef.current) rightScrollRef.current.scrollLeft = 0
    if (paneRef.current) {
      paneRef.current.style.setProperty('--dv-left-scroll', '0px')
      paneRef.current.style.setProperty('--dv-right-scroll', '0px')
    }
  }, [rows, paneRef])

  const onLeftScroll = useCallback(
    (e: React.UIEvent<HTMLDivElement>) => {
      if (paneRef.current) {
        paneRef.current.style.setProperty(
          '--dv-left-scroll',
          `${e.currentTarget.scrollLeft}px`
        )
      }
    },
    [paneRef]
  )
  const onRightScroll = useCallback(
    (e: React.UIEvent<HTMLDivElement>) => {
      if (paneRef.current) {
        paneRef.current.style.setProperty(
          '--dv-right-scroll',
          `${e.currentTarget.scrollLeft}px`
        )
      }
    },
    [paneRef]
  )

  const syncScrollCssVars = useCallback(() => {
    if (!paneRef.current) return
    paneRef.current.style.setProperty(
      '--dv-left-scroll',
      `${leftScrollRef.current?.scrollLeft ?? 0}px`
    )
    paneRef.current.style.setProperty(
      '--dv-right-scroll',
      `${rightScrollRef.current?.scrollLeft ?? 0}px`
    )
  }, [paneRef])

  const handleHorizontalWheel = useCallback(
    (e: React.WheelEvent<HTMLDivElement>) => {
      const delta = getHorizontalTrackpadDelta({
        deltaX: e.deltaX,
        deltaY: e.deltaY,
        shiftKey: e.shiftKey
      })
      if (delta === 0) return
      const explicitSide = (e.target as HTMLElement | null)?.closest<HTMLElement>(
        '[data-dv-scroll-side]'
      )
      const side =
        explicitSide?.dataset.dvScrollSide ??
        (() => {
          const pane = paneRef.current
          if (!pane) return 'right'
          const rect = pane.getBoundingClientRect()
          return e.clientX < rect.left + rect.width / 2 ? 'left' : 'right'
        })()
      const scrollEl =
        side === 'left' ? leftScrollRef.current : rightScrollRef.current
      if (!scrollEl) return
      scrollEl.scrollLeft += delta
      syncScrollCssVars()
      e.preventDefault()
    },
    [paneRef, syncScrollCssVars]
  )

  return (
    <div className="dv-scroll-region" onWheelCapture={handleHorizontalWheel}>
    <div ref={containerRef} className="dv-file-rows">
      {topPad > 0 && <div style={{ height: topPad }} aria-hidden="true" />}
      {rows.slice(start, end).map((row, idx) => {
        const i = start + idx
        if (row.kind === 'hunk') {
          return (
            <div key={i} className="dv-row dv-hunk-row">
              <span className="dv-hunk-text">{row.text}</span>
            </div>
          )
        }
        const left = row.left
        const right = row.right
        const thisIsChange = isChangeRow(row)
        const prev = i > 0 ? rows[i - 1] : null
        const prevIsChange = prev ? isChangeRow(prev) : false
        const isChangeStart = thisIsChange && !prevIsChange
        const activeLeftRangeIndex =
          activeSearchHit &&
          activeSearchHit.rowIndex === i &&
          activeSearchHit.side === 'left'
            ? activeSearchHit.rangeIndex
            : null
        const activeRightRangeIndex =
          activeSearchHit &&
          activeSearchHit.rowIndex === i &&
          activeSearchHit.side === 'right'
            ? activeSearchHit.rangeIndex
            : null
        const rowAnns = right?.newLine
          ? (annotationsByLine.get(right.newLine) ?? [])
          : []
        const hasExpandedAnn = rowAnns.some((a) => !collapsedAnnotationIds.has(a.id))
        return (
          <div
            key={i}
            className={`dv-row ${hasExpandedAnn ? 'dv-row-expanded-ann' : ''}`}
            data-row-idx={i}
            data-change-start={isChangeStart ? 'true' : undefined}
          >
            <div
              className={`dv-cell dv-cell-left ${
                left?.kind === 'del'
                  ? 'dv-del'
                  : left
                    ? 'dv-context'
                    : 'dv-empty'
              }`}
              data-dv-scroll-side="left"
            >
              <span className="dv-gutter">{left?.oldLine ?? ''}</span>
              <span className="dv-sign">
                {left?.kind === 'del' ? '-' : left ? ' ' : ''}
              </span>
              <span className="dv-content-clip">
                <span className="dv-content">
                  {left
                    ? renderHighlightedText(
                        left.text,
                        searchQuery,
                        activeLeftRangeIndex
                      )
                    : ''}
                </span>
              </span>
            </div>
            <div
              className={`dv-cell dv-cell-right ${
                right?.kind === 'add'
                  ? 'dv-add'
                  : right
                    ? 'dv-context'
                    : 'dv-empty'
              }`}
              data-dv-scroll-side="right"
              data-side="right"
              data-file={filePath}
              data-line={right?.newLine ?? ''}
            >
              <span className="dv-gutter">{right?.newLine ?? ''}</span>
              <span className="dv-sign">
                {right?.kind === 'add' ? '+' : right ? ' ' : ''}
              </span>
              <span className="dv-content-clip">
                <span className="dv-content">
                  {right
                    ? renderHighlightedText(
                        right.text,
                        searchQuery,
                        activeRightRangeIndex
                      )
                    : ''}
                </span>
              </span>
              {rowAnns.length > 0 && (
                <div className="dv-inline-anns">
                  {rowAnns.map((a) => {
                    const expanded = !collapsedAnnotationIds.has(a.id)
                    return (
                      <div
                        key={a.id}
                        className={`dv-inline-ann ${expanded ? 'expanded' : ''}`}
                      >
                        <button
                          className="dv-inline-ann-icon"
                          onClick={() => onToggleAnnotation(a.id)}
                          title={expanded ? '折叠标注' : '展开标注'}
                        >
                          {expanded ? '▾' : '▸'}
                        </button>
                        {expanded && (
                          <div className="dv-inline-ann-panel">
                            <div className="dv-inline-ann-head">
                              <span className="dv-inline-ann-loc">{a.lineRange}</span>
                              <div className="dv-inline-ann-actions">
                                <button
                                  className="dv-inline-ann-action"
                                  onClick={() => onEditAnnotation(a)}
                                >
                                  编辑
                                </button>
                                <button
                                  className="dv-inline-ann-action danger"
                                  onClick={() => onDeleteAnnotation(a.id)}
                                >
                                  删除
                                </button>
                              </div>
                            </div>
                            <div className="dv-inline-ann-body">{a.comment}</div>
                          </div>
                        )}
                      </div>
                    )
                  })}
                </div>
              )}
            </div>
          </div>
        )
      })}
      {bottomPad > 0 && <div style={{ height: bottomPad }} aria-hidden="true" />}
    </div>
    <div className="dv-hscroll-row" aria-hidden="true">
      <div
        ref={leftScrollRef}
        className="dv-hscroll-side"
        data-dv-scroll-side="left"
        onScroll={onLeftScroll}
      >
        <div
          className="dv-hscroll-track"
          style={{ width: `calc(${maxLeftChars}ch + 74px)` }}
        />
      </div>
      <div
        ref={rightScrollRef}
        className="dv-hscroll-side"
        data-dv-scroll-side="right"
        onScroll={onRightScroll}
      >
        <div
          className="dv-hscroll-track"
          style={{ width: `calc(${maxRightChars}ch + 74px)` }}
        />
      </div>
    </div>
    </div>
  )
}

/** Parse a unified diff text (git diff output) into a per-file structure. */
function parseUnifiedDiff(text: string): DiffFile[] {
  const files: DiffFile[] = []
  let cur: DiffFile | null = null
  let oldLn = 0
  let newLn = 0
  for (const raw of text.split('\n')) {
    if (raw.startsWith('diff --git ')) {
      const m = raw.match(/^diff --git a\/(.+?) b\/(.+)$/)
      cur = { path: m?.[2] ?? raw, header: [raw], lines: [] }
      files.push(cur)
      oldLn = 0
      newLn = 0
      continue
    }
    if (!cur) continue
    if (raw.startsWith('@@')) {
      const m = raw.match(/^@@ -(\d+)(?:,\d+)? \+(\d+)(?:,\d+)? @@/)
      if (m) {
        oldLn = parseInt(m[1], 10)
        newLn = parseInt(m[2], 10)
      }
      cur.lines.push({ kind: 'hunk', text: raw })
      continue
    }
    if (
      raw.startsWith('+++ ') ||
      raw.startsWith('--- ') ||
      raw.startsWith('index ') ||
      raw.startsWith('new file') ||
      raw.startsWith('deleted file') ||
      raw.startsWith('similarity ') ||
      raw.startsWith('rename ') ||
      raw.startsWith('Binary files ')
    ) {
      cur.header.push(raw)
      continue
    }
    if (raw.startsWith('+')) {
      cur.lines.push({ kind: 'add', text: raw.slice(1), newLine: newLn })
      newLn++
    } else if (raw.startsWith('-')) {
      cur.lines.push({ kind: 'del', text: raw.slice(1), oldLine: oldLn })
      oldLn++
    } else if (raw.length === 0 || raw.startsWith(' ')) {
      cur.lines.push({
        kind: 'context',
        text: raw.length > 0 ? raw.slice(1) : '',
        oldLine: oldLn,
        newLine: newLn
      })
      oldLn++
      newLn++
    }
  }
  return files
}

function buildDiffFileTree(paths: string[]): DiffFileTreeNode[] {
  type MutableDir = {
    name: string
    fullPath: string
    dirs: Map<string, MutableDir>
    files: Set<string>
  }
  const root: MutableDir = {
    name: '',
    fullPath: '',
    dirs: new Map(),
    files: new Set()
  }
  for (const p of paths) {
    const parts = p.split('/').filter(Boolean)
    let cur = root
    for (let i = 0; i < parts.length; i++) {
      const seg = parts[i]
      const isLast = i === parts.length - 1
      if (isLast) {
        cur.files.add(seg)
      } else {
        const nextFull = cur.fullPath ? `${cur.fullPath}/${seg}` : seg
        const next =
          cur.dirs.get(seg) ??
          (() => {
            const d: MutableDir = {
              name: seg,
              fullPath: nextFull,
              dirs: new Map(),
              files: new Set()
            }
            cur.dirs.set(seg, d)
            return d
          })()
        cur = next
      }
    }
  }
  const toNodes = (dir: MutableDir): DiffFileTreeNode[] => {
    const dirs = Array.from(dir.dirs.values())
      .sort((a, b) => a.name.localeCompare(b.name))
      .map((d) => ({
        name: d.name,
        fullPath: d.fullPath,
        type: 'dir' as const,
        children: toNodes(d)
      }))
    const files = Array.from(dir.files)
      .sort((a, b) => a.localeCompare(b))
      .map((name) => ({
        name,
        fullPath: dir.fullPath ? `${dir.fullPath}/${name}` : name,
        type: 'file' as const
      }))
    return [...dirs, ...files]
  }
  return toNodes(root)
}

function collectDiffTreeDirs(nodes: DiffFileTreeNode[]): string[] {
  const out: string[] = []
  const walk = (list: DiffFileTreeNode[]): void => {
    for (const n of list) {
      if (n.type === 'dir') {
        out.push(n.fullPath)
        if (n.children) walk(n.children)
      }
    }
  }
  walk(nodes)
  return out
}

function parentDirs(path: string): string[] {
  const parts = path.split('/').filter(Boolean)
  const out: string[] = []
  let cur = ''
  for (let i = 0; i < parts.length - 1; i++) {
    cur = cur ? `${cur}/${parts[i]}` : parts[i]
    out.push(cur)
  }
  return out
}

function parseLineRangeStart(lineRange: string): number | null {
  const m = lineRange.match(/^(\d+)(?:-\d+)?$/)
  if (!m) return null
  const n = parseInt(m[1], 10)
  return Number.isFinite(n) && n > 0 ? n : null
}

export default function DiffViewerDialog({
  cwd,
  title,
  onClose,
  onSubmit,
  sessionRunning = true,
  annotations,
  onAnnotationsChange,
  generalNote,
  onGeneralNoteChange,
  mode,
  onModeChange,
  selectedCommit,
  onSelectedCommitChange,
  selectedFile,
  onSelectedFileChange
}: DiffViewerDialogProps) {
  const setMode = useCallback(
    (next: SetStateAction<DiffMode>) =>
      applyControlledStateUpdate(onModeChange, next),
    [onModeChange]
  )
  const setSelectedCommit = useCallback(
    (next: SetStateAction<string>) =>
      applyControlledStateUpdate(onSelectedCommitChange, next),
    [onSelectedCommitChange]
  )
  const setSelectedFile = useCallback(
    (next: SetStateAction<string>) =>
      applyControlledStateUpdate(onSelectedFileChange, next),
    [onSelectedFileChange]
  )

  const [commits, setCommits] = useState<CommitEntry[] | null>(null)
  const [commitsLoading, setCommitsLoading] = useState(false)

  const [diffText, setDiffText] = useState<string>('')
  const [diffLoading, setDiffLoading] = useState(false)
  const [diffError, setDiffError] = useState<string | null>(null)

  // Controlled: annotations + generalNote live in the parent so unsent
  // batches persist across dialog close/reopen. Parent is responsible for
  // clearing after a successful submit.
  const setAnnotations = useCallback(
    (updater: SetStateAction<DiffAnnotation[]>) => {
      applyControlledStateUpdate(onAnnotationsChange, updater)
    },
    [onAnnotationsChange]
  )
  const setGeneralNote = useCallback(
    (next: SetStateAction<string>) =>
      applyControlledStateUpdate(onGeneralNoteChange, next),
    [onGeneralNoteChange]
  )
  const [submitting, setSubmitting] = useState(false)
  const [searchOpen, setSearchOpen] = useState(false)
  const [searchQuery, setSearchQuery] = useState('')
  const [activeSearchHit, setActiveSearchHit] = useState(0)
  const [expandedDirs, setExpandedDirs] = useState<Set<string>>(new Set())
  const [collapsedAnnotationIds, setCollapsedAnnotationIds] = useState<Set<string>>(
    new Set()
  )
  const [treePaneWidth, setTreePaneWidth] = useState(260)
  const [sidePaneWidth, setSidePaneWidth] = useState(280)
  const [codeLeftPercent, setCodeLeftPercent] = useState(50)

  const [draft, setDraft] = useState<{
    file: string
    lineRange: string
    snippet: string
    x: number
    y: number
  } | null>(null)
  const [composer, setComposer] = useState<{
    file: string
    lineRange: string
    snippet: string
    comment: string
    editingId: string | null
  } | null>(null)

  const diffPaneRef = useRef<HTMLDivElement | null>(null)
  const searchInputRef = useRef<HTMLInputElement | null>(null)
  const dragRef = useRef<{
    kind: 'tree' | 'side' | 'code' | null
    startX: number
    startValue: number
    paneWidth: number
  }>({ kind: null, startX: 0, startValue: 0, paneWidth: 0 })

  // Defer the heavy diff render so mode switches / dropdown clicks / close
  // button remain responsive while a big diff is being laid out. React will
  // render the expensive file list in a low-priority transition and keep
  // the UI's interactive inputs on the high-priority path.
  const deferredDiffText = useDeferredValue(diffText)
  const files = useMemo(
    () => parseUnifiedDiff(deferredDiffText),
    [deferredDiffText]
  )
  const fileTree = useMemo(
    () => buildDiffFileTree(files.map((f) => f.path)),
    [files]
  )
  // Defer the dropdown selection too — switching files re-mounts thousands
  // of DOM rows; without this, the dropdown itself freezes while React
  // commits the new file's DOM.
  const deferredSelectedFile = useDeferredValue(selectedFile)
  const visibleFiles = useMemo(
    () =>
      deferredSelectedFile
        ? files.filter((f) => f.path === deferredSelectedFile)
        : files,
    [files, deferredSelectedFile]
  )

  useEffect(() => {
    if (files.length === 0) return
    setExpandedDirs((prev) => {
      const next = new Set(prev)
      const selected = files.some((f) => f.path === selectedFile)
        ? selectedFile
        : files[0].path
      for (const dir of parentDirs(selected)) next.add(dir)
      return next
    })
  }, [files, selectedFile])

  // Pre-compute paired rows + change-start indices for the single visible
  // file. Keeping these at the top level lets jumpHunk work against virtual
  // indices (not DOM), so it stays correct even when rows are off-screen.
  const currentFile = visibleFiles[0]
  const currentRows = useMemo<PairedRow[]>(
    () => (currentFile ? pairLines(currentFile.lines) : []),
    [currentFile]
  )
  const changeStartIndices = useMemo<number[]>(() => {
    const out: number[] = []
    const isChange = (r: PairedRow): boolean =>
      r.kind === 'pair' && (r.left?.kind === 'del' || r.right?.kind === 'add')
    let prevChange = false
    for (let i = 0; i < currentRows.length; i++) {
      const cur = isChange(currentRows[i])
      if (cur && !prevChange) out.push(i)
      prevChange = cur
    }
    return out
  }, [currentRows])

  const searchHits = useMemo<SearchHit[]>(() => {
    const q = searchQuery.trim()
    if (!q) return []
    const out: SearchHit[] = []
    for (let i = 0; i < currentRows.length; i++) {
      const row = currentRows[i]
      if (row.kind !== 'pair') continue
      if (row.left) {
        const leftRanges = findMatchRanges(row.left.text, q)
        for (let j = 0; j < leftRanges.length; j++) {
          out.push({ rowIndex: i, side: 'left', rangeIndex: j })
        }
      }
      if (row.right) {
        const rightRanges = findMatchRanges(row.right.text, q)
        for (let j = 0; j < rightRanges.length; j++) {
          out.push({ rowIndex: i, side: 'right', rangeIndex: j })
        }
      }
    }
    return out
  }, [currentRows, searchQuery])

  const activeSearchMatch =
    searchHits.length > 0
      ? searchHits[Math.min(activeSearchHit, searchHits.length - 1)]
      : null

  const scrollToRow = useCallback(
    (rowIdx: number, behavior: ScrollBehavior = 'smooth') => {
      const pane = diffPaneRef.current
      if (!pane) return
      const rowsContainer = pane.querySelector<HTMLDivElement>('.dv-file-rows')
      if (!rowsContainer) return
      const paneRect = pane.getBoundingClientRect()
      const ctRect = rowsContainer.getBoundingClientRect()
      const containerTopInPane = ctRect.top - paneRect.top + pane.scrollTop
      const targetTop =
        containerTopInPane + rowIdx * DV_ROW_H - pane.clientHeight / 2 + DV_ROW_H
      pane.scrollTo({ top: Math.max(0, targetTop), behavior })
    },
    []
  )

  const openSearch = useCallback(() => {
    setSearchOpen(true)
    requestAnimationFrame(() => {
      searchInputRef.current?.focus()
      searchInputRef.current?.select()
    })
  }, [])

  const jumpSearch = useCallback(
    (dir: 'next' | 'prev') => {
      if (searchHits.length === 0) return
      const n = searchHits.length
      const next =
        dir === 'next'
          ? (activeSearchHit + 1) % n
          : (activeSearchHit - 1 + n) % n
      setActiveSearchHit(next)
      scrollToRow(searchHits[next].rowIndex)
    },
    [activeSearchHit, scrollToRow, searchHits]
  )

  useEffect(() => {
    if (searchHits.length === 0) {
      setActiveSearchHit(0)
      return
    }
    setActiveSearchHit((prev) => Math.min(prev, searchHits.length - 1))
  }, [searchHits])

  useEffect(() => {
    if (!searchQuery.trim()) return
    if (searchHits.length === 0) return
    const hit = searchHits[Math.min(activeSearchHit, searchHits.length - 1)]
    scrollToRow(hit.rowIndex, 'auto')
  }, [activeSearchHit, scrollToRow, searchHits, searchQuery])

  // Keep selection valid when files change (mode switch / refresh).
  // Do NOT clear when files is empty — refreshDiff() transiently empties
  // files between "loading" and "loaded", and clearing here would wipe the
  // persisted selection the parent holds across close/reopen.
  useEffect(() => {
    if (files.length === 0) return
    if (!files.some((f) => f.path === selectedFile)) {
      setSelectedFile(files[0].path)
    }
  }, [files, selectedFile])

  // Load commit history on mount (and whenever cwd changes).
  useEffect(() => {
    let cancelled = false
    setCommitsLoading(true)
    void window.api.git.log(cwd, 80).then((res) => {
      if (cancelled) return
      setCommitsLoading(false)
      if (res.ok && res.entries) {
        setCommits(res.entries)
        const stillExists = res.entries.some((e) => e.hash === selectedCommit)
        if (!stillExists && res.entries[0]) setSelectedCommit(res.entries[0].hash)
      } else {
        setCommits([])
      }
    })
    return () => {
      cancelled = true
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [cwd])

  const refreshDiff = useCallback(async () => {
    setDiffLoading(true)
    setDiffError(null)
    setDiffText('')
    let refs: string[] | undefined
    if (mode === 'commit') {
      if (!selectedCommit) {
        setDiffError('请先选择一个 commit')
        setDiffLoading(false)
        return
      }
      refs = [selectedCommit]
    }
    const res = await window.api.git.diff(cwd, mode, refs)
    setDiffLoading(false)
    if (res.ok) {
      setDiffText(res.diff ?? '')
    } else {
      setDiffError(res.error ?? '获取 diff 失败')
    }
  }, [cwd, mode, selectedCommit])

  // Auto-load diff when mode or refs change.
  useEffect(() => {
    void refreshDiff()
  }, [refreshDiff])

  /** Scroll to the next / previous change block within the CURRENT file
   *  view. "Change block" = start of a contiguous add/del run (marked with
   *  data-change-start in the row DOM). */
  const jumpHunk = useCallback(
    (dir: 'next' | 'prev') => {
      const pane = diffPaneRef.current
      if (!pane || changeStartIndices.length === 0) return
      // Locate the `.dv-file-rows` container inside the pane so we can
      // compute each change block's absolute scroll offset from its row
      // index (DOM query won't work — virtualization leaves most rows
      // unmounted).
      const rowsContainer = pane.querySelector<HTMLDivElement>('.dv-file-rows')
      if (!rowsContainer) return
      const paneRect = pane.getBoundingClientRect()
      const ctRect = rowsContainer.getBoundingClientRect()
      const containerTopInPane =
        ctRect.top - paneRect.top + pane.scrollTop
      const cur = pane.scrollTop
      const EPS = 8
      const targetY = (idx: number): number =>
        containerTopInPane + idx * DV_ROW_H - pane.clientHeight / 2 + DV_ROW_H
      let targetIdx: number | null = null
      if (dir === 'next') {
        targetIdx =
          changeStartIndices.find((i) => targetY(i) > cur + EPS) ??
          changeStartIndices[0]
      } else {
        for (let i = changeStartIndices.length - 1; i >= 0; i--) {
          if (targetY(changeStartIndices[i]) < cur - EPS) {
            targetIdx = changeStartIndices[i]
            break
          }
        }
        if (targetIdx == null)
          targetIdx = changeStartIndices[changeStartIndices.length - 1]
      }
      if (targetIdx != null) {
        pane.scrollTo({ top: Math.max(0, targetY(targetIdx)), behavior: 'smooth' })
      }
    },
    [changeStartIndices]
  )

  // Capture text selection inside the RIGHT (new-code) column only —
  // annotations anchor on the modified side, matching user workflow.
  //
  // Geometry-based detection: iterate every rendered `.dv-row` in the pane
  // and include any whose bounding rect overlaps the selection's rect
  // vertically. For each included row, pull file+line off its right cell.
  // Avoids fragility with `range.startContainer/endContainer` when the drag
  // crosses rows (gutter/sign columns are user-select:none, which can make
  // endpoint-based checks fall into the gaps).
  const handleMouseUp = useCallback(() => {
    const sel = window.getSelection()
    if (!sel || sel.isCollapsed || !diffPaneRef.current) {
      setDraft(null)
      return
    }
    const range = sel.getRangeAt(0)
    const pane = diffPaneRef.current
    if (!pane.contains(range.commonAncestorContainer)) {
      setDraft(null)
      return
    }
    const selRect = range.getBoundingClientRect()
    if (selRect.width === 0 && selRect.height === 0) {
      setDraft(null)
      return
    }
    const rows = pane.querySelectorAll<HTMLElement>(
      '.dv-row:not(.dv-hunk-row)'
    )
    let touchedFile = ''
    let loLine = Infinity
    let hiLine = -Infinity
    let touchedRows = 0
    for (const row of Array.from(rows)) {
      const r = row.getBoundingClientRect()
      if (r.bottom <= selRect.top) continue
      if (r.top >= selRect.bottom) continue
      const rightCell = row.querySelector<HTMLElement>(
        '.dv-cell-right[data-file]'
      )
      if (!rightCell) continue
      touchedRows++
      const lineAttr = rightCell.dataset.line
      if (!lineAttr) continue
      const ln = parseInt(lineAttr, 10)
      if (!Number.isFinite(ln) || ln <= 0) continue
      if (ln < loLine) loLine = ln
      if (ln > hiLine) hiLine = ln
      if (!touchedFile) touchedFile = rightCell.dataset.file ?? ''
    }
    if (import.meta.env?.DEV) {
      console.debug('[DiffViewer] mouseup', {
        touchedRows,
        touchedFile,
        loLine,
        hiLine,
        selRect: { top: selRect.top, bottom: selRect.bottom },
        rowsInPane: rows.length
      })
    }
    if (!touchedFile || loLine === Infinity) {
      setDraft(null)
      return
    }
    const lineRange = loLine === hiLine ? `${loLine}` : `${loLine}-${hiLine}`
    // Single-line: use the literal selected text so partial-line selections
    // anchor on the actual tokens the user highlighted.
    // Multi-line: rebuild from DOM, one line per row, so the composer shows
    // each line of code rather than a concatenated blob (`.dv-content`s are
    // inline spans, so `sel.toString()` drops line boundaries).
    let snippet: string
    if (loLine === hiLine) {
      snippet = sel.toString()
    } else {
      const escFile = touchedFile.replace(/"/g, '\\"')
      const collected: string[] = []
      for (let n = loLine; n <= hiLine; n++) {
        const cell = pane.querySelector<HTMLElement>(
          `.dv-cell-right[data-file="${escFile}"][data-line="${n}"]`
        )
        const content = cell?.querySelector<HTMLElement>('.dv-content')
        collected.push(content?.textContent ?? '')
      }
      snippet = collected.join('\n')
    }
    const paneRect = pane.getBoundingClientRect()
    const buttonPos = placeDraftButton({
      paneWidth: pane.clientWidth,
      paneScrollTop: pane.scrollTop,
      paneRectLeft: paneRect.left,
      paneRectTop: paneRect.top,
      selectionRectRight: selRect.right,
      selectionRectTop: selRect.top
    })
    setDraft({
      file: touchedFile,
      lineRange,
      snippet,
      x: buttonPos.x,
      y: buttonPos.y
    })
  }, [])

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'f') {
        e.preventDefault()
        openSearch()
        return
      }
      if (searchOpen && e.key === 'Enter') {
        e.preventDefault()
        jumpSearch(e.shiftKey ? 'prev' : 'next')
        return
      }
      if (e.key === 'Escape') {
        if (composer) setComposer(null)
        else if (draft) setDraft(null)
        else if (searchOpen) {
          if (searchQuery.trim()) setSearchQuery('')
          else setSearchOpen(false)
        }
        else {
          setDiffText('')
          requestAnimationFrame(() => onClose())
        }
      }
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [composer, draft, jumpSearch, onClose, openSearch, searchOpen, searchQuery])

  const openComposerFromDraft = useCallback(() => {
    if (!draft) return
    setComposer({
      file: draft.file,
      lineRange: draft.lineRange,
      snippet: draft.snippet,
      comment: '',
      editingId: null
    })
    setDraft(null)
  }, [draft])

  const saveComposer = useCallback(() => {
    if (!composer) return
    const c = composer.comment.trim()
    if (!c) return
    if (composer.editingId) {
      setAnnotations((prev) =>
        prev.map((a) => (a.id === composer.editingId ? { ...a, comment: c } : a))
      )
    } else {
      setAnnotations((prev) => [
        ...prev,
        {
          id: genId(),
          file: composer.file,
          lineRange: composer.lineRange,
          snippet: composer.snippet,
          comment: c
        }
      ])
    }
    setComposer(null)
  }, [composer])

  const editAnnotation = useCallback((a: DiffAnnotation) => {
    setComposer({
      file: a.file,
      lineRange: a.lineRange,
      snippet: a.snippet,
      comment: a.comment,
      editingId: a.id
    })
  }, [])

  const removeEditingAnnotation = useCallback(() => {
    if (!composer?.editingId) return
    const id = composer.editingId
    setAnnotations((prev) => prev.filter((a) => a.id !== id))
    setCollapsedAnnotationIds((prev) => {
      if (!prev.has(id)) return prev
      const next = new Set(prev)
      next.delete(id)
      return next
    })
    setComposer(null)
  }, [composer])

  const removeAnnotation = useCallback((id: string) => {
    setAnnotations((prev) => prev.filter((a) => a.id !== id))
    setCollapsedAnnotationIds((prev) => {
      if (!prev.has(id)) return prev
      const next = new Set(prev)
      next.delete(id)
      return next
    })
  }, [])

  const toggleInlineAnnotation = useCallback((id: string) => {
    setCollapsedAnnotationIds((prev) => {
      const next = new Set(prev)
      if (next.has(id)) next.delete(id)
      else next.add(id)
      return next
    })
  }, [])

  const pendingJumpRef = useRef<{ file: string; startLine: number } | null>(null)

  const findRowIndexForLine = useCallback(
    (rows: PairedRow[], startLine: number): number => {
      for (let i = 0; i < rows.length; i++) {
        const r = rows[i]
        if (r.kind !== 'pair') continue
        if (r.right?.newLine === startLine) return i
      }
      return -1
    },
    []
  )

  const jumpToAnnotation = useCallback(
    (a: DiffAnnotation) => {
      const start = parseLineRangeStart(a.lineRange)
      if (!start) return
      // Always make sure the annotation card itself is expanded so the user
      // sees the text right after the scroll lands.
      setCollapsedAnnotationIds((prev) => {
        if (!prev.has(a.id)) return prev
        const next = new Set(prev)
        next.delete(a.id)
        return next
      })
      if (a.file !== selectedFile) {
        pendingJumpRef.current = { file: a.file, startLine: start }
        setSelectedFile(a.file)
        return
      }
      const idx = findRowIndexForLine(currentRows, start)
      if (idx >= 0) scrollToRow(idx)
    },
    [
      currentRows,
      findRowIndexForLine,
      scrollToRow,
      selectedFile,
      setSelectedFile
    ]
  )

  useEffect(() => {
    const pending = pendingJumpRef.current
    if (!pending) return
    if (!currentFile || currentFile.path !== pending.file) return
    if (currentRows.length === 0) return
    const idx = findRowIndexForLine(currentRows, pending.startLine)
    pendingJumpRef.current = null
    if (idx < 0) return
    // Wait one frame so virtualized rows + DOM layout settle.
    requestAnimationFrame(() => scrollToRow(idx))
  }, [currentFile, currentRows, findRowIndexForLine, scrollToRow])

  const annotationsByLine = useMemo(() => {
    const map = new Map<number, DiffAnnotation[]>()
    if (!currentFile) return map
    for (const a of annotations) {
      if (a.file !== currentFile.path) continue
      const ln = parseLineRangeStart(a.lineRange)
      if (!ln) continue
      const arr = map.get(ln) ?? []
      arr.push(a)
      map.set(ln, arr)
    }
    return map
  }, [annotations, currentFile])

  /** Close flow that clears the big diff DOM before unmounting the dialog,
   *  so the close click feels instant even when a 10k-line diff is loaded. */
  const handleSmoothClose = useCallback(() => {
    setDiffText('')
    // Give the renderer one frame to shrink the DOM, then unmount the modal.
    requestAnimationFrame(() => onClose())
  }, [onClose])

  const canSubmit = !submitting && annotations.length > 0

  const handleSubmit = useCallback(async () => {
    if (!canSubmit) return
    setSubmitting(true)
    try {
      await onSubmit(annotations, '')
    } finally {
      setSubmitting(false)
    }
  }, [canSubmit, annotations, onSubmit])

  const toggleDir = useCallback((dirPath: string) => {
    setExpandedDirs((prev) => {
      const next = new Set(prev)
      if (next.has(dirPath)) next.delete(dirPath)
      else next.add(dirPath)
      return next
    })
  }, [])

  const allTreeDirs = useMemo(() => collectDiffTreeDirs(fileTree), [fileTree])
  const expandAllDirs = useCallback(() => {
    setExpandedDirs(new Set(allTreeDirs))
  }, [allTreeDirs])
  const collapseAllDirs = useCallback(() => {
    setExpandedDirs(new Set())
  }, [])
  const allDirsExpanded =
    allTreeDirs.length > 0 &&
    allTreeDirs.every((d) => expandedDirs.has(d))

  const renderTreeNodes = useCallback(
    (nodes: DiffFileTreeNode[], depth = 0): JSX.Element[] =>
      nodes.map((node) => {
        if (node.type === 'dir') {
          const expanded = expandedDirs.has(node.fullPath)
          return (
            <li key={node.fullPath} className="dv-tree-node">
              <button
                className="dv-tree-dir"
                style={{ paddingLeft: `${8 + depth * 14}px` }}
                onClick={() => toggleDir(node.fullPath)}
              >
                <span className="dv-tree-caret">{expanded ? '▾' : '▸'}</span>
                <span className="dv-tree-name">{node.name}</span>
              </button>
              {expanded && node.children && node.children.length > 0 && (
                <ul className="dv-tree-list">
                  {renderTreeNodes(node.children, depth + 1)}
                </ul>
              )}
            </li>
          )
        }
        const active = node.fullPath === selectedFile
        return (
          <li key={node.fullPath} className="dv-tree-node">
            <button
              className={`dv-tree-file ${active ? 'active' : ''}`}
              style={{ paddingLeft: `${28 + depth * 14}px` }}
              onClick={() => setSelectedFile(node.fullPath)}
              title={node.fullPath}
            >
              <span className="dv-tree-name">{node.name}</span>
            </button>
          </li>
        )
      }),
    [expandedDirs, selectedFile, setSelectedFile, toggleDir]
  )

  useEffect(() => {
    const onMove = (e: MouseEvent) => {
      const drag = dragRef.current
      if (!drag.kind) return
      const dx = e.clientX - drag.startX
      if (drag.kind === 'tree') {
        setTreePaneWidth(Math.max(180, Math.min(520, drag.startValue + dx)))
        return
      }
      if (drag.kind === 'side') {
        setSidePaneWidth(Math.max(220, Math.min(560, drag.startValue - dx)))
        return
      }
      if (drag.kind === 'code' && drag.paneWidth > 0) {
        const next = Math.max(
          22,
          Math.min(78, drag.startValue + (dx / drag.paneWidth) * 100)
        )
        setCodeLeftPercent(next)
      }
    }
    const onUp = () => {
      if (!dragRef.current.kind) return
      dragRef.current.kind = null
      document.body.style.userSelect = ''
      document.body.style.cursor = ''
    }
    window.addEventListener('mousemove', onMove)
    window.addEventListener('mouseup', onUp)
    return () => {
      window.removeEventListener('mousemove', onMove)
      window.removeEventListener('mouseup', onUp)
    }
  }, [])

  const startTreeResize = useCallback(
    (e: React.MouseEvent) => {
      e.preventDefault()
      dragRef.current = {
        kind: 'tree',
        startX: e.clientX,
        startValue: treePaneWidth,
        paneWidth: 0
      }
      document.body.style.userSelect = 'none'
      document.body.style.cursor = 'col-resize'
    },
    [treePaneWidth]
  )

  const startSideResize = useCallback(
    (e: React.MouseEvent) => {
      e.preventDefault()
      dragRef.current = {
        kind: 'side',
        startX: e.clientX,
        startValue: sidePaneWidth,
        paneWidth: 0
      }
      document.body.style.userSelect = 'none'
      document.body.style.cursor = 'col-resize'
    },
    [sidePaneWidth]
  )

  const startCodeResize = useCallback(
    (e: React.MouseEvent) => {
      e.preventDefault()
      dragRef.current = {
        kind: 'code',
        startX: e.clientX,
        startValue: codeLeftPercent,
        paneWidth: diffPaneRef.current?.clientWidth ?? 0
      }
      document.body.style.userSelect = 'none'
      document.body.style.cursor = 'col-resize'
    },
    [codeLeftPercent]
  )

  const bodyStyle = useMemo(
    () =>
      ({
        '--dv-tree-w': `${treePaneWidth}px`,
        '--dv-side-w': `${sidePaneWidth}px`
      }) as CSSProperties,
    [treePaneWidth, sidePaneWidth]
  )

  const diffPaneStyle = useMemo(
    () =>
      ({
        '--dv-code-left': `${codeLeftPercent}%`
      }) as CSSProperties,
    [codeLeftPercent]
  )

  return (
    <div className="modal-backdrop" onClick={handleSmoothClose}>
      <div
        className="modal diff-viewer-modal"
        onClick={(e) => e.stopPropagation()}
      >
        <div className="modal-head">
          <h3>{title ?? 'Diff 审查 · 代码标注回灌给当前会话'}</h3>
          <button className="modal-close" onClick={handleSmoothClose}>
            ×
          </button>
        </div>

        <div className="dv-toolbar">
          <div className="dv-mode-tabs">
            {DIFF_MODE_TABS.map((m) => (
              <button
                key={m}
                className={`dv-mode-tab ${mode === m ? 'active' : ''}`}
                onClick={() => setMode(m)}
              >
                {diffModeLabel(m)}
              </button>
            ))}
          </div>
          {mode === 'commit' && (
            <div className="dv-ref-select">
              <label>commit：</label>
              <select
                value={selectedCommit}
                onChange={(e) => setSelectedCommit(e.target.value)}
                disabled={commitsLoading}
              >
                {commitsLoading ? (
                  <option>加载中…</option>
                ) : commits && commits.length > 0 ? (
                  commits.map((c) => (
                    <option key={c.hash} value={c.hash}>
                      {c.short} · {c.subject.slice(0, 60)} · {c.author}
                    </option>
                  ))
                ) : (
                  <option>（无 commit）</option>
                )}
              </select>
            </div>
          )}
          <div className="dv-nav-group">
            <button
              className="dv-nav-btn"
              onClick={openSearch}
              title="Search in current file (Ctrl+F)"
            >
              Find
            </button>
            <button
              className="dv-nav-btn"
              onClick={() => jumpHunk('prev')}
              disabled={diffLoading || visibleFiles.length === 0}
              title="跳到上一个改动块"
            >
              ↑ 上一个
            </button>
            <button
              className="dv-nav-btn"
              onClick={() => jumpHunk('next')}
              disabled={diffLoading || visibleFiles.length === 0}
              title="跳到下一个改动块"
            >
              ↓ 下一个
            </button>
          </div>
          {searchOpen && (
            <div className="dv-search">
              <input
                ref={searchInputRef}
                className="dv-search-input"
                value={searchQuery}
                onChange={(e) => setSearchQuery(e.target.value)}
                placeholder="Search current file (Ctrl+F)"
              />
              <span className="dv-search-count">
                {searchQuery.trim()
                  ? `${searchHits.length === 0 ? 0 : activeSearchHit + 1}/${searchHits.length}`
                  : '0/0'}
              </span>
              <button
                className="dv-nav-btn"
                onClick={() => jumpSearch('prev')}
                disabled={searchHits.length === 0}
                title="Previous match (Shift+Enter)"
              >
                Prev
              </button>
              <button
                className="dv-nav-btn"
                onClick={() => jumpSearch('next')}
                disabled={searchHits.length === 0}
                title="Next match (Enter)"
              >
                Next
              </button>
              <button
                className="dv-nav-btn"
                onClick={() => {
                  setSearchQuery('')
                  setSearchOpen(false)
                }}
                title="Close search (Esc)"
              >
                Close
              </button>
            </div>
          )}
          <button
            className="dv-refresh-btn"
            onClick={() => void refreshDiff()}
            disabled={diffLoading}
            title="重新拉取 diff"
          >
            {diffLoading ? '…' : '⟳'}
          </button>
        </div>
        {/* file tree moved to left pane */}
        <div className="dv-body" style={bodyStyle}>
          <aside className="dv-tree-pane">
            <div className="dv-tree-head">
              <span className="dv-tree-head-title">
                Changed Files <span className="dv-file-count">{files.length}</span>
              </span>
              {allTreeDirs.length > 0 && (
                <button
                  type="button"
                  className="dv-tree-head-toggle"
                  onClick={allDirsExpanded ? collapseAllDirs : expandAllDirs}
                  title={allDirsExpanded ? '全部收起' : '全部展开'}
                >
                  {allDirsExpanded ? '全部收起' : '全部展开'}
                </button>
              )}
            </div>
            {files.length === 0 ? (
              <div className="dv-tree-empty">No changed files</div>
            ) : (
              <ul className="dv-tree-list">{renderTreeNodes(fileTree)}</ul>
            )}
          </aside>
          <div
            className="dv-pane-splitter"
            role="separator"
            aria-orientation="vertical"
            onMouseDown={startTreeResize}
            title="拖拽调整文件树宽度"
          />
          <div
            className="dv-diff-pane"
            ref={diffPaneRef}
            onMouseUp={handleMouseUp}
            style={diffPaneStyle}
          >
            {diffLoading && <div className="dv-placeholder">正在加载 diff…</div>}
            {!diffLoading && diffError && (
              <div className="dv-error">⚠ {diffError}</div>
            )}
            {!diffLoading && !diffError && files.length === 0 && (
              <div className="dv-placeholder">
                （此源没有变更 —— 换一个 commit 或模式试试）
              </div>
            )}
            {!diffLoading && !diffError && currentFile && (
              <div className="dv-file">
                <div className="dv-file-head">📄 {currentFile.path}</div>
                <VirtualizedFileRows
                  filePath={currentFile.path}
                  rows={currentRows}
                  paneRef={diffPaneRef}
                  searchQuery={searchQuery.trim()}
                  activeSearchHit={activeSearchMatch}
                  annotationsByLine={annotationsByLine}
                  onEditAnnotation={editAnnotation}
                  collapsedAnnotationIds={collapsedAnnotationIds}
                  onToggleAnnotation={toggleInlineAnnotation}
                  onDeleteAnnotation={removeAnnotation}
                />
                <div
                  className="dv-code-splitter"
                  role="separator"
                  aria-orientation="vertical"
                  style={{ left: `calc(${codeLeftPercent}% - 3px)` }}
                  onMouseDown={startCodeResize}
                  title="拖拽调整左右代码列宽度"
                />
              </div>
            )}

            {draft && (
              <button
                className="plan-review-annotate-floater"
                style={{ left: draft.x, top: draft.y }}
                onClick={openComposerFromDraft}
                title="对这段代码添加标注"
              >
                ✏ 标注
              </button>
            )}
          </div>
          <div
            className="dv-pane-splitter"
            role="separator"
            aria-orientation="vertical"
            onMouseDown={startSideResize}
            title="拖拽调整批注栏宽度"
          />

          <aside className="dv-side">
            <div className="dv-side-head">
              <span className="dv-tree-head-title">
                批注 <span className="dv-file-count">{annotations.length}</span>
              </span>
            </div>
            {annotations.length === 0 ? (
              <div className="dv-side-empty-msg">
                暂无批注 — 在右侧代码区选中文本后点击「✏ 标注」添加
              </div>
            ) : (
              <ul className="dv-ann-list">
                {annotations.map((a) => {
                  const active = a.file === selectedFile
                  return (
                    <li key={a.id} className={`dv-ann-item ${active ? 'active' : ''}`}>
                      <button
                        type="button"
                        className="dv-ann-item-jump"
                        onClick={() => jumpToAnnotation(a)}
                        title={`跳转到 ${a.file}:${a.lineRange}`}
                      >
                        <span className="dv-ann-item-file">{a.file}</span>
                        <span className="dv-ann-item-lr">:{a.lineRange}</span>
                      </button>
                      <div className="dv-ann-item-comment">{a.comment}</div>
                      <div className="dv-ann-item-actions">
                        <button
                          className="dv-inline-ann-action"
                          onClick={() => editAnnotation(a)}
                        >
                          编辑
                        </button>
                        <button
                          className="dv-inline-ann-action danger"
                          onClick={() => removeAnnotation(a.id)}
                        >
                          删除
                        </button>
                      </div>
                    </li>
                  )
                })}
              </ul>
            )}
          </aside>
        </div>

        {composer && (
          <div
            className="plan-review-composer-backdrop"
            onClick={() => setComposer(null)}
          >
            <div
              className="plan-review-composer"
              onClick={(e) => e.stopPropagation()}
            >
              <div className="plan-review-composer-head">
                {composer.editingId ? '编辑批注' : '添加批注'} ·{' '}
                <span className="dv-ann-location">
                  {composer.file}:{composer.lineRange}
                </span>
              </div>
              <pre className="dv-ann-snippet">
                {composer.snippet.length > 600
                  ? composer.snippet.slice(0, 600) + '…'
                  : composer.snippet}
              </pre>
              <textarea
                className="plan-review-composer-input"
                value={composer.comment}
                onChange={(e) =>
                  setComposer({ ...composer, comment: e.target.value })
                }
                autoFocus
                placeholder="这段代码哪里不对 / 需要怎么改 / 遗漏了什么..."
                rows={5}
              />
              <div className="plan-review-composer-actions">
                <button
                  className="drawer-btn"
                  onClick={() => setComposer(null)}
                >
                  取消
                </button>
                {composer.editingId && (
                  <button
                    className="drawer-btn danger"
                    onClick={removeEditingAnnotation}
                  >
                    Delete
                  </button>
                )}
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

        <div className="drawer-actions plan-review-actions">
          <span style={{ flex: 1 }} />
          {!sessionRunning && (
            <div
              className="modal-error"
              style={{ background: 'transparent', color: 'var(--mac-fg-subtle)' }}
            >
              会话未启动，请先启动会话再发送批注
            </div>
          )}
          <button
            className="drawer-btn primary"
            disabled={!sessionRunning || annotations.length === 0}
            onClick={handleSubmit}
            title={
              !sessionRunning
                ? '会话未启动 — 先启动会话再发送批注'
                : undefined
            }
          >
            发送到会话 ({annotations.length} 条批注)
          </button>
        </div>
      </div>
    </div>
  )
}
