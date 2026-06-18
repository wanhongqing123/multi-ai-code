import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it } from 'vitest'
import SkillMarkdownPreview from './SkillMarkdownPreview'

describe('SkillMarkdownPreview', () => {
  it('renders SKILL.md content as markdown instead of plain preformatted text', () => {
    const markup = renderToStaticMarkup(
      <SkillMarkdownPreview
        markdown={[
          '# Skill Title',
          '',
          'Use this before **important** work.',
          '',
          '- inspect context',
          '- apply changes',
          '',
          '```ts',
          'const enabled = true',
          '```'
        ].join('\n')}
      />
    )

    expect(markup).toContain('<h1>Skill Title</h1>')
    expect(markup).toContain('<strong>important</strong>')
    expect(markup).toContain('<li>inspect context</li>')
    expect(markup).toContain('class="language-ts"')
    expect(markup).toContain('const enabled = true')
    expect(markup).not.toContain('<pre># Skill Title')
  })
})
