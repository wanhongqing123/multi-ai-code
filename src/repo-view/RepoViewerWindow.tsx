import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { type AiSettings } from '../components/AiSettingsDialog'
import AnalysisPanel, { type RepoCodeAnnotation } from './AnalysisPanel'
import CodePane, { type RepoSelection } from './CodePane'
import FileTree from './FileTree'
import { parseAnalysisOutput } from './parseAnalysisOutput'
import { buildRepoAnnotationMessage } from './repoAnnotationMessage.js'
import {
  createUserMessage,
  syncAssistantMessage,
  type RepoConversationMessage
} from './repoConversation.js'

function cleanTerminalChunk(raw: string): string {
  return raw
    .replace(/\u001B\][\s\S]*?(?:\u0007|\u001B\\)/g, '')
    .replace(/\u001B\[[0-?]*[ -/]*[@-~]/g, '')
    .replace(/\r/g, '')
}

type RecentTopic = { at: string; filePath: string; topic: string }

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
  const [analysisPending, setAnalysisPending] = useState(false)
  const [analysisMessages, setAnalysisMessages] = useState<RepoConversationMessage[]>([])
  const [projectSummary, setProjectSummary] = useState('')
  const [fileNote, setFileNote] = useState('')
  const [recentTopics, setRecentTopics] = useState<RecentTopic[]>([])
  const pendingMemoryFileRef = useRef<string | null>(null)
  const analysisRawRef = useRef('')

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
    if (!project) return
    void window.api.repoView.memoryLoad(project.target_repo).then((res) => {
      if (!res.ok) return
      setProjectSummary(res.summary ?? '')
      setRecentTopics((res.recentTopics ?? []) as RecentTopic[])
    })
  }, [project])

  useEffect(() => {
    setSelectedFile('')
    setSelectedContent('')
    setSelectedSize(0)
    setAnnotations([])
    setEditingAnnotationId(null)
    setAnalysisMessages([])
    setProjectSummary('')
    setFileNote('')
    setRecentTopics([])
  }, [projectId])

  useEffect(() => {
    if (!project || !selectedFile) return
    let cancelled = false
    setLoadingFile(true)
    void Promise.all([
      window.api.repoView.readFile(project.target_repo, selectedFile),
      window.api.repoView.memoryFileNote(project.target_repo, selectedFile)
    ]).then(([readRes, noteRes]) => {
      if (cancelled) return
      setLoadingFile(false)
      if (!readRes.ok || readRes.content === undefined) {
        setSelectedContent(readRes.error ?? '无法读取文件')
        setSelectedSize(0)
      } else {
        setSelectedContent(readRes.content)
        setSelectedSize(readRes.byteLength ?? 0)
      }
      setFileNote(noteRes.ok ? noteRes.fileNote ?? '' : '')
    })
    return () => {
      cancelled = true
    }
  }, [project, selectedFile])

  useEffect(() => {
    setEditingAnnotationId(null)
  }, [selectedFile])

  useEffect(() => {
    const offData = window.api.repoView.onAnalysisData((evt) => {
      const chunk = cleanTerminalChunk(evt.chunk)
      if (!chunk) return
      analysisRawRef.current = (analysisRawRef.current + chunk).slice(-220000)
      const parsed = parseAnalysisOutput(analysisRawRef.current)
      if (parsed.answer.trim()) {
        setAnalysisMessages((prev) =>
          syncAssistantMessage(prev, parsed.answer, !parsed.complete)
        )
      }
      if (!parsed.complete || !project) return
      const pendingFile = pendingMemoryFileRef.current
      pendingMemoryFileRef.current = null
      setAnalysisPending(false)
      if (!pendingFile || !parsed.memoryUpdate.trim()) return
      void window.api.repoView
        .memoryApply(project.target_repo, pendingFile, parsed.memoryUpdate)
        .then((res) => {
          if (!res.ok) return
          setProjectSummary(res.summary ?? '')
          if (pendingFile === selectedFile) {
            setFileNote(res.fileNote ?? '')
          }
          setRecentTopics((res.recentTopics ?? []) as RecentTopic[])
        })
    })
    const offStatus = window.api.repoView.onAnalysisStatus((evt) => {
      if (evt.status === 'exited') {
        setAnalysisPending(false)
      }
    })
    return () => {
      offData()
      offStatus()
    }
  }, [project, selectedFile])

  useEffect(() => {
    return () => {
      void window.api.repoView.analysisStop()
    }
  }, [])

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

  const onSendAnalysis = useCallback(
    async (question: string) => {
      if (!project || !selectedFile) return
      const targetAnns = annotations.filter((a) => a.filePath === selectedFile)
      if (targetAnns.length === 0) return
      const command = repoViewSettings.command ?? repoViewSettings.ai_cli
      const defaultArgs = command === 'codex' ? ['--full-auto'] : []
      const args = [...defaultArgs, ...(repoViewSettings.args ?? [])]
      const startRes = await window.api.repoView.analysisStart({
        projectId,
        targetRepo: project.target_repo,
        command,
        args,
        env: repoViewSettings.env ?? {}
      })
      if (!startRes.ok) {
        setAnalysisMessages((prev) =>
          syncAssistantMessage(prev, `分析会话启动失败：${startRes.error ?? '未知错误'}`, false)
        )
        return
      }

      const selection = buildRepoAnnotationMessage({
        filePath: selectedFile,
        question,
        annotations: targetAnns
      })

      pendingMemoryFileRef.current = selectedFile
      analysisRawRef.current = ''
      setAnalysisPending(true)
      setAnalysisMessages((prev) => [
        ...prev,
        createUserMessage({
          filePath: selectedFile,
          annotationCount: targetAnns.length,
          question
        })
      ])
      const sendRes = await window.api.repoView.analysisSend({
        repoRoot: project.target_repo,
        filePath: selectedFile,
        selection,
        question: '',
        projectSummary,
        fileNote
      })
      if (!sendRes.ok) {
        pendingMemoryFileRef.current = null
        setAnalysisPending(false)
        setAnalysisMessages((prev) =>
          syncAssistantMessage(prev, `分析请求发送失败：${sendRes.error ?? '未知错误'}`, false)
        )
      }
    },
    [annotations, fileNote, project, projectId, projectSummary, repoViewSettings, selectedFile]
  )

  if (!project) {
    return <div className="repo-view-empty">项目不存在或尚未加载</div>
  }

  return (
    <div className="repo-view-window">
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
      <aside className="repo-view-analysis">
        <AnalysisPanel
          projectId={projectId}
          repoRoot={project.target_repo}
          filePath={selectedFile}
          annotations={annotations.filter((a) => a.filePath === selectedFile)}
          aiCli={repoViewSettings.ai_cli}
          running={analysisPending}
          messages={analysisMessages}
          recentTopics={recentTopics}
          onSendAnalysis={onSendAnalysis}
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
      </aside>
    </div>
  )
}
