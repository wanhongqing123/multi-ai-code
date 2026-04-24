import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { type AiSettings } from '../components/AiSettingsDialog'
import AnalysisPanel, { type RepoCodeAnnotation } from './AnalysisPanel'
import CodePane, { type RepoSelection } from './CodePane'
import FileTree from './FileTree'
import RepoTerminalPanel from './RepoTerminalPanel'
import { buildCliInjectionText } from './buildCliInjectionText'

const TERMINAL_SPLIT_MIN = 120
const TERMINAL_SPLIT_MAX_RATIO = 0.75
const TERMINAL_SPLIT_STORAGE_KEY = 'repo-view:terminal-split-px'
const ANALYSIS_WIDTH_MIN = 280
const ANALYSIS_WIDTH_MAX = 900
const ANALYSIS_WIDTH_STORAGE_KEY = 'repo-view:analysis-width-px'


export default function RepoViewerWindow({
  projectId
}: {
  projectId: string
}): JSX.Element {
  const [projects, setProjects] = useState<
    Array<{ id: string; name: string; target_repo: string }>
  >([])
  const [selectedFile, setSelectedFile] = useState('')
  const [selectedContent, setSelectedContent] = useState('')
  const [selectedSize, setSelectedSize] = useState(0)
  const [loadingFile, setLoadingFile] = useState(false)
  const [repoViewSettings, setRepoViewSettings] = useState<AiSettings>({ ai_cli: 'claude' })
  const [annotations, setAnnotations] = useState<RepoCodeAnnotation[]>([])
  const [editingAnnotationId, setEditingAnnotationId] = useState<string | null>(null)
  const [sessionRunning, setSessionRunning] = useState(false)

  const project = useMemo(
    () => projects.find((p) => p.id === projectId) ?? null,
    [projects, projectId]
  )

  useEffect(() => {
    void window.api.project.list().then((list) => {
      setProjects(
        list.map((p) => ({
          id: p.id,
          name: p.name,
          target_repo: p.target_repo
        }))
      )
    })
    void window.api.project.getRepoViewAiSettings(projectId).then((settings) => {
      setRepoViewSettings(settings)
    })
  }, [projectId])


  useEffect(() => {
    setSelectedFile('')
    setSelectedContent('')
    setSelectedSize(0)
    setAnnotations([])
    setEditingAnnotationId(null)
  }, [projectId])

  useEffect(() => {
    if (!project || !selectedFile) return
    let cancelled = false
    setLoadingFile(true)
    void window.api.repoView
      .readFile(project.target_repo, selectedFile)
      .then((readRes) => {
        if (cancelled) return
        setLoadingFile(false)
        if (!readRes.ok || readRes.content === undefined) {
          setSelectedContent(readRes.error ?? '无法读取文件')
          setSelectedSize(0)
        } else {
          setSelectedContent(readRes.content)
          setSelectedSize(readRes.byteLength ?? 0)
        }
      })
    return () => {
      cancelled = true
    }
  }, [project, selectedFile])

  useEffect(() => {
    setEditingAnnotationId(null)
  }, [selectedFile])

  useEffect(() => {
    const offStatus = window.api.repoView.onAnalysisStatus((evt) => {
      if (evt.status === 'running') {
        setSessionRunning(true)
      } else if (evt.status === 'exited') {
        setSessionRunning(false)
      }
    })
    return () => {
      offStatus()
    }
  }, [])

  useEffect(() => {
    void window.api.repoView.analysisHas().then((res) => {
      if (res.ok && res.running) setSessionRunning(true)
    })
    return () => {
      void window.api.repoView.analysisStop()
    }
  }, [])

  const onStartCli = useCallback(async (): Promise<boolean> => {
    if (!project) return false
    const command = repoViewSettings.command ?? repoViewSettings.ai_cli
    const defaultArgs = command === 'codex' ? ['--full-auto'] : []
    const args = [...defaultArgs, ...(repoViewSettings.args ?? [])]
    const res = await window.api.repoView.analysisStart({
      projectId,
      targetRepo: project.target_repo,
      command,
      args,
      env: repoViewSettings.env ?? {}
    })
    if (res.ok) {
      setSessionRunning(true)
      return true
    }
    return false
  }, [project, projectId, repoViewSettings])

  const onStopCli = useCallback(() => {
    void window.api.repoView.analysisStop().then(() => {
      setSessionRunning(false)
    })
  }, [])

  const [terminalHeight, setTerminalHeight] = useState<number>(() => {
    const saved = Number(localStorage.getItem(TERMINAL_SPLIT_STORAGE_KEY))
    return Number.isFinite(saved) && saved > TERMINAL_SPLIT_MIN ? saved : 260
  })
  const splitContainerRef = useRef<HTMLDivElement>(null)
  const dragStartRef = useRef<{ y: number; height: number } | null>(null)

  const onSplitMouseDown = useCallback(
    (e: React.MouseEvent) => {
      e.preventDefault()
      dragStartRef.current = { y: e.clientY, height: terminalHeight }
      const onMove = (ev: MouseEvent) => {
        if (!dragStartRef.current || !splitContainerRef.current) return
        const delta = dragStartRef.current.y - ev.clientY
        const total = splitContainerRef.current.clientHeight
        const maxH = Math.max(
          TERMINAL_SPLIT_MIN,
          Math.floor(total * TERMINAL_SPLIT_MAX_RATIO)
        )
        const next = Math.min(
          maxH,
          Math.max(TERMINAL_SPLIT_MIN, dragStartRef.current.height + delta)
        )
        setTerminalHeight(next)
      }
      const onUp = () => {
        dragStartRef.current = null
        window.removeEventListener('mousemove', onMove)
        window.removeEventListener('mouseup', onUp)
        try {
          localStorage.setItem(TERMINAL_SPLIT_STORAGE_KEY, String(terminalHeight))
        } catch {
          /* ignore */
        }
      }
      window.addEventListener('mousemove', onMove)
      window.addEventListener('mouseup', onUp)
    },
    [terminalHeight]
  )

  useEffect(() => {
    try {
      localStorage.setItem(TERMINAL_SPLIT_STORAGE_KEY, String(terminalHeight))
    } catch {
      /* ignore */
    }
  }, [terminalHeight])

  const [analysisWidth, setAnalysisWidth] = useState<number>(() => {
    const saved = Number(localStorage.getItem(ANALYSIS_WIDTH_STORAGE_KEY))
    return Number.isFinite(saved) && saved >= ANALYSIS_WIDTH_MIN ? saved : 360
  })
  const widthDragRef = useRef<{ x: number; width: number } | null>(null)

  const onWidthMouseDown = useCallback(
    (e: React.MouseEvent) => {
      e.preventDefault()
      widthDragRef.current = { x: e.clientX, width: analysisWidth }
      const onMove = (ev: MouseEvent) => {
        if (!widthDragRef.current) return
        const delta = widthDragRef.current.x - ev.clientX
        const maxW = Math.min(
          ANALYSIS_WIDTH_MAX,
          Math.max(ANALYSIS_WIDTH_MIN, window.innerWidth - 400)
        )
        const next = Math.min(
          maxW,
          Math.max(ANALYSIS_WIDTH_MIN, widthDragRef.current.width + delta)
        )
        setAnalysisWidth(next)
      }
      const onUp = () => {
        widthDragRef.current = null
        window.removeEventListener('mousemove', onMove)
        window.removeEventListener('mouseup', onUp)
      }
      window.addEventListener('mousemove', onMove)
      window.addEventListener('mouseup', onUp)
    },
    [analysisWidth]
  )

  useEffect(() => {
    try {
      localStorage.setItem(ANALYSIS_WIDTH_STORAGE_KEY, String(analysisWidth))
    } catch {
      /* ignore */
    }
  }, [analysisWidth])


  const onAnnotateSelection = useCallback(
    (selection: RepoSelection, comment: string, editingId?: string) => {
      if (!selectedFile) return
      if (editingId) {
        setAnnotations((prev) =>
          prev.map((annotation) =>
            annotation.id === editingId
              ? {
                  ...annotation,
                  lineRange: selection.lineRange,
                  snippet: selection.snippet,
                  comment
                }
              : annotation
          )
        )
        setEditingAnnotationId(null)
        return
      }
      setAnnotations((prev) => [
        ...prev,
        {
          id: `ann_${Date.now().toString(36)}_${Math.random().toString(36).slice(2, 6)}`,
          filePath: selectedFile,
          lineRange: selection.lineRange,
          snippet: selection.snippet,
          comment
        }
      ])
    },
    [selectedFile]
  )

  const onSendToCli = useCallback(
    async (question: string) => {
      if (!project || !selectedFile) return
      const targetAnns = annotations.filter((a) => a.filePath === selectedFile)
      if (targetAnns.length === 0) return
      if (!sessionRunning) {
        const ok = await onStartCli()
        if (!ok) return
      }
      const text = buildCliInjectionText({
        repoRoot: project.target_repo,
        filePath: selectedFile,
        annotations: targetAnns,
        question
      })
      const res = await window.api.repoView.analysisSend({
        repoRoot: project.target_repo,
        text
      })
      if (!res.ok) {
        console.warn('[repo-view] analysisSend failed:', res.error)
      }
    },
    [annotations, project, selectedFile, sessionRunning, onStartCli]
  )

  if (!project) {
    return <div className="repo-view-empty">项目不存在或尚未加载</div>
  }

  return (
    <div
      className="repo-view-window"
      style={{
        gridTemplateColumns: `300px minmax(0, 1fr) 6px ${analysisWidth}px`
      }}
    >
      <aside className="repo-view-sidebar">
        <FileTree
          repoRoot={project.target_repo}
          selectedFile={selectedFile}
          onSelectFile={setSelectedFile}
        />
      </aside>
      <main className="repo-view-main">
        <CodePane
          filePath={selectedFile}
          content={selectedContent}
          byteLength={selectedSize}
          loading={loadingFile}
          onAnnotateSelection={onAnnotateSelection}
          editingAnnotation={
            annotations.find((annotation) => annotation.id === editingAnnotationId) ?? null
          }
          onCancelEditing={() => setEditingAnnotationId(null)}
        />
      </main>
      <div
        className="repo-view-width-divider"
        onMouseDown={onWidthMouseDown}
        role="separator"
        aria-orientation="vertical"
        title="拖拽调整右侧面板宽度"
      />
      <aside className="repo-view-analysis" ref={splitContainerRef}>
        <div className="repo-view-analysis-top">
          <AnalysisPanel
            filePath={selectedFile}
            annotations={annotations.filter((a) => a.filePath === selectedFile)}
            onSendToCli={onSendToCli}
            onEditAnnotation={(id) => setEditingAnnotationId(id)}
            onRemoveAnnotation={(id) => {
              if (editingAnnotationId === id) setEditingAnnotationId(null)
              setAnnotations((prev) => prev.filter((a) => a.id !== id))
            }}
            onClearAnnotations={() => {
              setEditingAnnotationId(null)
              setAnnotations((prev) => prev.filter((a) => a.filePath !== selectedFile))
            }}
          />
        </div>
        <div
          className="repo-view-analysis-divider"
          onMouseDown={onSplitMouseDown}
          role="separator"
          aria-orientation="horizontal"
          title="拖拽调整 AI CLI 面板高度"
        />
        <div
          className="repo-view-analysis-bottom"
          style={{ height: terminalHeight }}
        >
          <RepoTerminalPanel
            cliLabel={repoViewSettings.command ?? repoViewSettings.ai_cli}
            running={sessionRunning}
            onStart={() => void onStartCli()}
            onStop={onStopCli}
          />
        </div>
      </aside>
    </div>
  )
}
