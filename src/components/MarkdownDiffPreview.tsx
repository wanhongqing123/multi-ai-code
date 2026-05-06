import ReactMarkdown from 'react-markdown'
import remarkGfm from 'remark-gfm'

export interface MarkdownDiffPreviewProps {
  filePath: string
  oldText: string
  newText: string
}

function renderMarkdownBody(text: string, emptyLabel: string): JSX.Element {
  if (!text.trim()) {
    return <div className="dv-md-preview-empty">{emptyLabel}</div>
  }

  return (
    <div className="dv-md-preview-body md-rendered">
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
        {text}
      </ReactMarkdown>
    </div>
  )
}

export default function MarkdownDiffPreview({
  filePath,
  oldText,
  newText
}: MarkdownDiffPreviewProps): JSX.Element {
  return (
    <section className="dv-md-preview" aria-label={`Markdown Preview: ${filePath}`}>
      <div className="dv-md-preview-title">Markdown Preview</div>
      <div className="dv-md-preview-grid">
        <section className="dv-md-preview-col">
          <div className="dv-md-preview-head">Old</div>
          {renderMarkdownBody(oldText, 'No previous content')}
        </section>
        <section className="dv-md-preview-col">
          <div className="dv-md-preview-head">New</div>
          {renderMarkdownBody(newText, 'No current content')}
        </section>
      </div>
    </section>
  )
}
