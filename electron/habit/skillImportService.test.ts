import { afterEach, beforeEach, describe, expect, test } from 'vitest'
import { mkdir, mkdtemp, rm, writeFile } from 'fs/promises'
import { join } from 'path'
import { tmpdir } from 'os'
import { discoverSkillMarkdownFiles, parseSkillMarkdown } from './skillImportService.js'

let root: string

beforeEach(async () => {
  root = await mkdtemp(join(tmpdir(), 'mac-skill-import-'))
})

afterEach(async () => {
  await rm(root, { recursive: true, force: true })
})

describe('skill import service', () => {
  test('parses Claude-style SKILL.md frontmatter into an imported skill draft', () => {
    const draft = parseSkillMarkdown(
      `---
name: Systematic Debugging
description: Use when tests fail
arguments: issue
---

# Systematic Debugging

Follow the debugging process.
`,
      'systematic-debugging'
    )

    expect(draft.name).toBe('Systematic Debugging')
    expect(draft.description).toBe('Use when tests fail')
    expect(draft.trigger).toBe('systematic-debugging')
    expect(draft.steps).toEqual([
      {
        type: 'prompt',
        text: expect.stringContaining('# Systematic Debugging')
      },
      { type: 'wait-response' }
    ])
  })

  test('discovers every child folder that contains SKILL.md', async () => {
    await mkdir(join(root, 'debugging'), { recursive: true })
    await mkdir(join(root, 'writing'), { recursive: true })
    await mkdir(join(root, 'not-a-skill'), { recursive: true })
    await writeFile(join(root, 'debugging', 'SKILL.md'), '# Debugging')
    await writeFile(join(root, 'writing', 'SKILL.md'), '# Writing')

    const found = await discoverSkillMarkdownFiles(root)

    expect(found.map((item) => item.name)).toEqual(['debugging', 'writing'])
  })
})
