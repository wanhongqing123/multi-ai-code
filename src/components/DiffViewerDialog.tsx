import {
  useCallback,
  useDeferredValue,
  useEffect,
  useMemo,
  useRef,
  useState
} from 'react'

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
  onAnnotationsChange: (next: DiffAnnotation[]) => void
  /** Controlled general-note field — same persistence rationale as annotations. */
  generalNote: string
  onGeneralNoteChange: (next: string) => void
}

type DiffMode = 'working' | 'head1' | 'commit' | 'range' | 'working_range'

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
  paneRef
}: {
  filePath: string
  rows: PairedRow[]
  paneRef: React.RefObject<HTMLDivElement | null>
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

  return (
    <>
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
        return (
          <div
            key={i}
            className="dv-row"
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
            >
              <span className="dv-gutter">{left?.oldLine ?? ''}</span>
              <span className="dv-sign">
                {left?.kind === 'del' ? '-' : left ? ' ' : ''}
              </span>
              <span className="dv-content-clip">
                <span className="dv-content">{left ? left.text : ''}</span>
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
              data-side="right"
              data-file={filePath}
              data-line={right?.newLine ?? ''}
            >
              <span className="dv-gutter">{right?.newLine ?? ''}</span>
              <span className="dv-sign">
                {right?.kind === 'add' ? '+' : right ? ' ' : ''}
              </span>
              <span className="dv-content-clip">
                <span className="dv-content">{right ? right.text : ''}</span>
              </span>
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
        onScroll={onRightScroll}
      >
        <div
          className="dv-hscroll-track"
          style={{ width: `calc(${maxRightChars}ch + 74px)` }}
        />
      </div>
    </div>
    </>
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

export default function DiffViewerDialog({
  cwd,
  title,
  onClose,
  onSubmit,
  sessionRunning = true,
  annotations,
  onAnnotationsChange,
  generalNote,
  onGeneralNoteChange
}: DiffViewerDialogProps) {
  const [mode, setMode] = useState<DiffMode>('working')
  const [commits, setCommits] = useState<CommitEntry[] | null>(null)
  const [commitsLoading, setCommitsLoading] = useState(false)
  const [selectedCommit, setSelectedCommit] = useState<string>('')
  const [rangeFrom, setRangeFrom] = useState<string>('')
  const [rangeTo, setRangeTo] = useState<string>('')
  /** "最近 N 次 commit + 当前改动" — internally maps to `git diff HEAD~N`. */
  const [workingRangeCount, setWorkingRangeCount] = useState<number>(3)

  const [diffText, setDiffText] = useState<string>('')
  const [diffLoading, setDiffLoading] = useState(false)
  const [diffError, setDiffError] = useState<string | null>(null)

  // Which file's diff is currently shown. When files[] changes (user flipped
  // mode / re-fetched), default to first file if the previous selection is
  // no longer present.
  const [selectedFile, setSelectedFile] = useState<string>('')

  // Controlled: annotations + generalNote live in the parent so unsent
  // batches persist across dialog close/reopen. Parent is responsible for
  // clearing after a successful submit.
  const setAnnotations = useCallback(
    (
      updater:
        | DiffAnnotation[]
        | ((prev: DiffAnnotation[]) => DiffAnnotation[])
    ) => {
      const next =
        typeof updater === 'function'
          ? (updater as (prev: DiffAnnotation[]) => DiffAnnotation[])(annotations)
          : updater
      onAnnotationsChange(next)
    },
    [annotations, onAnnotationsChange]
  )
  const setGeneralNote = useCallback(
    (next: string) => onGeneralNoteChange(next),
    [onGeneralNoteChange]
  )
  const [submitting, setSubmitting] = useState(false)

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

  // Defer the heavy diff render so mode switches / dropdown clicks / close
  // button remain responsive while a big diff is being laid out. React will
  // render the expensive file list in a low-priority transition and keep
  // the UI's interactive inputs on the high-priority path.
  const deferredDiffText = useDeferredValue(diffText)
  const files = useMemo(
    () => parseUnifiedDiff(deferredDiffText),
    [deferredDiffText]
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

  // Keep selection valid when files change (mode switch / refresh).
  useEffect(() => {
    if (files.length === 0) {
      if (selectedFile) setSelectedFile('')
      return
    }
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
        if (!selectedCommit && res.entries[0]) setSelectedCommit(res.entries[0].hash)
        if (!rangeTo && res.entries[0]) setRangeTo(res.entries[0].hash)
        if (!rangeFrom && res.entries[1]) setRangeFrom(res.entries[1].hash)
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
    } else if (mode === 'range') {
      if (!rangeFrom || !rangeTo) {
        setDiffError('请选择范围的起止 commit')
        setDiffLoading(false)
        return
      }
      refs = [rangeFrom, rangeTo]
    } else if (mode === 'working_range') {
      if (!workingRangeCount || workingRangeCount < 1) {
        setDiffError('最近提交次数必须 ≥ 1')
        setDiffLoading(false)
        return
      }
      refs = [`HEAD~${workingRangeCount}`]
    }
    const res = await window.api.git.diff(cwd, mode, refs)
    setDiffLoading(false)
    if (res.ok) {
      setDiffText(res.diff ?? '')
    } else {
      setDiffError(res.error ?? '获取 diff 失败')
    }
  }, [cwd, mode, selectedCommit, rangeFrom, rangeTo, workingRangeCount])

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
  // Geometry-based detection: iterate every `.dv-cell-right` in the pane and
  // include any whose bounding rect overlaps the selection's rect vertically.
  // This avoids fragility with `sel.getRangeAt(0).startContainer/endContainer`
  // when the drag crosses rows (intermediate gutter/sign columns are
  // `user-select:none`, which can make a naive endpoint check fail).
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
    const rightCells = pane.querySelectorAll<HTMLElement>(
      '.dv-cell-right[data-file][data-line]'
    )
    let touchedFile = ''
    let loLine = Infinity
    let hiLine = -Infinity
    for (const cell of Array.from(rightCells)) {
      const r = cell.getBoundingClientRect()
      // Intersect vertically with the selection rect.
      if (r.bottom <= selRect.top) continue
      if (r.top >= selRect.bottom) continue
      const ln = parseInt(cell.dataset.line ?? '0', 10)
      if (!ln) continue
      if (ln < loLine) loLine = ln
      if (ln > hiLine) hiLine = ln
      if (!touchedFile) touchedFile = cell.dataset.file ?? ''
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
    setDraft({
      file: touchedFile,
      lineRange,
      snippet,
      x: selRect.right - paneRect.left + 4,
      y: selRect.top - paneRect.top + pane.scrollTop - 4
    })
  }, [])

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        if (composer) setComposer(null)
        else if (draft) setDraft(null)
        else {
          setDiffText('')
          requestAnimationFrame(() => onClose())
        }
      }
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [composer, draft, onClose])

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

  const removeAnnotation = useCallback((id: string) => {
    setAnnotations((prev) => prev.filter((a) => a.id !== id))
  }, [])

  const editAnnotation = useCallback((a: DiffAnnotation) => {
    setComposer({
      file: a.file,
      lineRange: a.lineRange,
      snippet: a.snippet,
      comment: a.comment,
      editingId: a.id
    })
  }, [])

  /** Close flow that clears the big diff DOM before unmounting the dialog,
   *  so the close click feels instant even when a 10k-line diff is loaded. */
  const handleSmoothClose = useCallback(() => {
    setDiffText('')
    // Give the renderer one frame to shrink the DOM, then unmount the modal.
    requestAnimationFrame(() => onClose())
  }, [onClose])

  const canSubmit =
    !submitting && (annotations.length > 0 || generalNote.trim().length > 0)

  const handleSubmit = useCallback(async () => {
    if (!canSubmit) return
    setSubmitting(true)
    try {
      await onSubmit(annotations, generalNote.trim())
    } finally {
      setSubmitting(false)
    }
  }, [canSubmit, annotations, generalNote, onSubmit])

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
            {(
              ['working', 'working_range', 'head1', 'commit', 'range'] as DiffMode[]
            ).map((m) => (
              <button
                key={m}
                className={`dv-mode-tab ${mode === m ? 'active' : ''}`}
                onClick={() => setMode(m)}
              >
                {m === 'working'
                  ? '📝 当前改动'
                  : m === 'working_range'
                    ? '📝+📐 当前 + 最近提交'
                    : m === 'head1'
                      ? '⏱ 最近一次 commit'
                      : m === 'commit'
                        ? '🎯 指定 commit'
                        : '📐 commit 范围'}
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
          {mode === 'range' && (
            <div className="dv-ref-select">
              <label>从：</label>
              <select
                value={rangeFrom}
                onChange={(e) => setRangeFrom(e.target.value)}
                disabled={commitsLoading}
              >
                {(commits ?? []).map((c) => (
                  <option key={c.hash} value={c.hash}>
                    {c.short} · {c.subject.slice(0, 40)}
                  </option>
                ))}
              </select>
              <label>到：</label>
              <select
                value={rangeTo}
                onChange={(e) => setRangeTo(e.target.value)}
                disabled={commitsLoading}
              >
                {(commits ?? []).map((c) => (
                  <option key={c.hash} value={c.hash}>
                    {c.short} · {c.subject.slice(0, 40)}
                  </option>
                ))}
              </select>
            </div>
          )}
          {mode === 'working_range' && (
            <div className="dv-ref-select">
              <label>最近</label>
              <input
                type="number"
                min={1}
                max={50}
                value={workingRangeCount}
                onChange={(e) =>
                  setWorkingRangeCount(
                    Math.max(1, Math.min(50, parseInt(e.target.value, 10) || 1))
                  )
                }
                className="dv-num-input"
                title="合并最近 N 次 commit 的改动 + 当前未提交改动（git diff HEAD~N）"
              />
              <label>次 commit + 当前改动</label>
            </div>
          )}
          <div className="dv-nav-group">
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
          <button
            className="dv-refresh-btn"
            onClick={() => void refreshDiff()}
            disabled={diffLoading}
            title="重新拉取 diff"
          >
            {diffLoading ? '…' : '⟳'}
          </button>
        </div>
        {files.length > 0 && (
          <div className="dv-file-picker">
            <label>文件：</label>
            <select
              value={selectedFile}
              onChange={(e) => setSelectedFile(e.target.value)}
              title="选择要查看的文件"
            >
              {files.map((f) => (
                <option key={f.path} value={f.path}>
                  {f.path}
                </option>
              ))}
            </select>
            <span className="dv-file-count">
              {files.length} 个文件有变更
            </span>
          </div>
        )}

        <div className="dv-body">
          <div
            className="dv-diff-pane"
            ref={diffPaneRef}
            onMouseUp={handleMouseUp}
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

          <aside className="dv-side">
            <div className="plan-review-side-title">
              批注（{annotations.length}）
            </div>
            {annotations.length === 0 ? (
              <div className="plan-review-empty">
                在左侧 diff 里选中代码行 → 点浮现的 "✏ 标注" 按钮。
                <br />
                也可以在下方写"整体意见"。
              </div>
            ) : (
              <ul className="plan-review-list">
                {annotations.map((a, i) => (
                  <li key={a.id} className="plan-review-item">
                    <div className="plan-review-item-head">
                      <span className="plan-review-item-idx">#{i + 1}</span>
                      <span className="dv-ann-location">
                        {a.file}:{a.lineRange}
                      </span>
                      <div className="plan-review-item-actions">
                        <button
                          className="plan-review-item-btn"
                          onClick={() => editAnnotation(a)}
                          title="编辑"
                        >
                          ✎
                        </button>
                        <button
                          className="plan-review-item-btn danger"
                          onClick={() => removeAnnotation(a.id)}
                          title="删除"
                        >
                          ✕
                        </button>
                      </div>
                    </div>
                    <pre className="dv-ann-snippet">
                      {a.snippet.length > 400
                        ? a.snippet.slice(0, 400) + '…'
                        : a.snippet}
                    </pre>
                    <div className="plan-review-item-comment">{a.comment}</div>
                  </li>
                ))}
              </ul>
            )}

            <div className="plan-review-general">
              <label className="plan-review-general-label">
                整体意见（可选）
              </label>
              <textarea
                className="plan-review-general-input"
                value={generalNote}
                onChange={(e) => setGeneralNote(e.target.value)}
                placeholder="对这次 diff 的整体看法、遗漏项、优先级..."
                rows={3}
              />
            </div>
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
          <button
            className="drawer-btn"
            onClick={handleSmoothClose}
            disabled={submitting}
          >
            ✕ 关闭（不发送）
          </button>
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
