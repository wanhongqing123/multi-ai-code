import { useCallback, useEffect, useMemo, useState } from 'react'

interface RepoTreeEntry {
  name: string
  path: string
  isDirectory: boolean
}

export interface FileTreeProps {
  repoRoot: string
  selectedFile: string
  onSelectFile: (path: string) => void
}

export default function FileTree({
  repoRoot,
  selectedFile,
  onSelectFile
}: FileTreeProps): JSX.Element {
  const [childrenByDir, setChildrenByDir] = useState<Record<string, RepoTreeEntry[]>>({})
  const [expanded, setExpanded] = useState<Set<string>>(() => new Set(['']))
  const [loadingDirs, setLoadingDirs] = useState<Set<string>>(() => new Set())
  const [error, setError] = useState<string | null>(null)

  const loadDir = useCallback(
    async (dir: string) => {
      setLoadingDirs((prev) => new Set(prev).add(dir))
      const res = await window.api.repoView.listTree(repoRoot, dir)
      setLoadingDirs((prev) => {
        const next = new Set(prev)
        next.delete(dir)
        return next
      })
      if (!res.ok) {
        setError(res.error ?? '读取目录失败')
        return
      }
      setError(null)
      setChildrenByDir((prev) => ({ ...prev, [dir]: res.entries }))
    },
    [repoRoot]
  )

  useEffect(() => {
    setChildrenByDir({})
    setExpanded(new Set(['']))
    setError(null)
    void loadDir('')
  }, [repoRoot, loadDir])

  const toggleDir = useCallback(
    (dir: string) => {
      setExpanded((prev) => {
        const next = new Set(prev)
        if (next.has(dir)) {
          next.delete(dir)
          return next
        }
        next.add(dir)
        if (!childrenByDir[dir]) void loadDir(dir)
        return next
      })
    },
    [childrenByDir, loadDir]
  )

  const renderDir = useCallback(
    (dir: string, depth: number): JSX.Element[] => {
      const entries = childrenByDir[dir] ?? []
      return entries.flatMap((entry) => {
        if (entry.isDirectory) {
          const isOpen = expanded.has(entry.path)
          const rows: JSX.Element[] = [
            <button
              key={`dir:${entry.path}`}
              className="repo-tree-row repo-tree-row-dir"
              style={{ paddingLeft: 12 + depth * 14 }}
              onClick={() => toggleDir(entry.path)}
              title={entry.path}
            >
              <span className="repo-tree-caret">{isOpen ? '▾' : '▸'}</span>
              <span className="repo-tree-name">📁 {entry.name}</span>
            </button>
          ]
          if (isOpen) {
            if (loadingDirs.has(entry.path)) {
              rows.push(
                <div
                  key={`loading:${entry.path}`}
                  className="repo-tree-loading"
                  style={{ paddingLeft: 24 + depth * 14 }}
                >
                  加载中…
                </div>
              )
            }
            rows.push(...renderDir(entry.path, depth + 1))
          }
          return rows
        }
        return [
          <button
            key={`file:${entry.path}`}
            className={`repo-tree-row repo-tree-row-file ${selectedFile === entry.path ? 'active' : ''}`}
            style={{ paddingLeft: 12 + depth * 14 }}
            onClick={() => onSelectFile(entry.path)}
            title={entry.path}
          >
            <span className="repo-tree-caret" />
            <span className="repo-tree-name">📄 {entry.name}</span>
          </button>
        ]
      })
    },
    [childrenByDir, expanded, loadingDirs, onSelectFile, selectedFile, toggleDir]
  )

  const rows = useMemo(() => renderDir('', 0), [renderDir])

  return (
    <div className="repo-tree">
      <div className="repo-tree-head">文件</div>
      {error && <div className="repo-tree-error">{error}</div>}
      {loadingDirs.has('') && !childrenByDir[''] ? (
        <div className="repo-tree-loading">加载中…</div>
      ) : (
        <div className="repo-tree-body">{rows}</div>
      )}
    </div>
  )
}
