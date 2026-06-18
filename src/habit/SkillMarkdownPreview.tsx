import ReactMarkdown from 'react-markdown'
import remarkGfm from 'remark-gfm'

interface Props {
  markdown: string
}

export default function SkillMarkdownPreview(props: Props): JSX.Element {
  const content = props.markdown.trim() || '无法读取 SKILL.md'

  return (
    <div className="skill-markdown-preview">
      <ReactMarkdown remarkPlugins={[remarkGfm]}>{content}</ReactMarkdown>
    </div>
  )
}
