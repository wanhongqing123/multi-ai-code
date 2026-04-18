import { useMemo } from 'react'
import ReactMarkdown from 'react-markdown'
import remarkGfm from 'remark-gfm'

export interface FilePreviewDialogProps {
  path: string
  content: string
  title?: string
  confirmLabel?: string
  onConfirm: () => void
  onCancel: () => void
}

function formatBytes(n: number): string {
  if (n < 1024) return `${n} B`
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`
  return `${(n / (1024 * 1024)).toFixed(1)} MB`
}

export default function FilePreviewDialog({
  path,
  content,
  title,
  confirmLabel,
  onConfirm,
  onCancel
}: FilePreviewDialogProps) {
  const lineCount = useMemo(() => content.split(/\r?\n/).length, [content])
  const byteCount = useMemo(() => new Blob([content]).size, [content])
  const filename = path.split(/[\\/]/).pop() ?? path

  return (
    <div className="modal-backdrop" onClick={onCancel}>
      <div
        className="modal file-preview-modal"
        onClick={(e) => e.stopPropagation()}
      >
        <div className="modal-head">
          <h3>{title ?? '预览外部方案文件'}</h3>
          <button className="modal-close" onClick={onCancel}>
            ×
          </button>
        </div>

        <div className="file-preview-meta" title={path}>
          <span className="file-preview-name">📄 {filename}</span>
          <span className="file-preview-sep">·</span>
          <span>{lineCount} 行</span>
          <span className="file-preview-sep">·</span>
          <span>{formatBytes(byteCount)}</span>
          <span className="file-preview-path">{path}</span>
        </div>

        <div className="file-preview-body md-rendered">
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

        <div className="drawer-actions file-preview-actions">
          <button className="drawer-btn" onClick={onCancel}>
            ✕ 取消
          </button>
          <span style={{ flex: 1 }} />
          <button className="drawer-btn primary" onClick={onConfirm}>
            {confirmLabel ?? '✓ 确认使用此方案'}
          </button>
        </div>
      </div>
    </div>
  )
}
