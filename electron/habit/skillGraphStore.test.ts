import { afterEach, beforeEach, describe, expect, test } from 'vitest'
import { mkdtemp, readFile, rm } from 'fs/promises'
import { join } from 'path'
import { tmpdir } from 'os'
import {
  deleteSkillPipeline,
  listSkillPipelines,
  readSkillPipeline,
  saveSkillPipeline,
  skillPipelineDir,
  validateSkillPipeline,
  type SkillPipeline
} from './skillGraphStore.js'

let targetRepo: string

function samplePipeline(): SkillPipeline {
  return {
    id: 'fix-test-failure',
    name: 'Fix test failure',
    description: 'Find root cause, write tests, then verify',
    nodes: [
      {
        id: 'input',
        kind: 'input',
        title: 'Input',
        position: { x: 40, y: 120 },
        inputs: [],
        outputs: [{ id: 'user_request', name: 'User request', dataType: 'text' }]
      },
      {
        id: 'debug',
        kind: 'skill',
        skillId: 'skill-debug',
        title: 'systematic-debugging',
        position: { x: 280, y: 120 },
        inputs: [{ id: 'issue', name: 'Issue', dataType: 'text' }],
        outputs: [{ id: 'root_cause', name: 'Root cause', dataType: 'text' }]
      }
    ],
    edges: [
      {
        id: 'edge-1',
        from: { nodeId: 'input', portId: 'user_request' },
        to: { nodeId: 'debug', portId: 'issue' }
      }
    ],
    createdAt: '2026-06-17T00:00:00.000Z',
    updatedAt: '2026-06-17T00:00:00.000Z'
  }
}

beforeEach(async () => {
  targetRepo = await mkdtemp(join(tmpdir(), 'mac-skill-pipelines-'))
})

afterEach(async () => {
  await rm(targetRepo, { recursive: true, force: true })
})

describe('skill graph project storage', () => {
  test('stores pipelines under the target repo .multi-ai-code folder', async () => {
    const dir = skillPipelineDir(targetRepo)

    expect(dir).toBe(join(targetRepo, '.multi-ai-code', 'skill-pipelines'))
  })

  test('saves, lists, reads, and deletes a project pipeline', async () => {
    const saved = await saveSkillPipeline(targetRepo, samplePipeline())

    expect(saved.ok).toBe(true)
    const list = await listSkillPipelines(targetRepo)
    expect(list).toEqual([
      {
        id: 'fix-test-failure',
        name: 'Fix test failure',
        description: 'Find root cause, write tests, then verify',
        nodeCount: 2,
        edgeCount: 1,
        updatedAt: expect.any(String)
      }
    ])

    const loaded = await readSkillPipeline(targetRepo, 'fix-test-failure')
    expect(loaded?.nodes.map((node) => node.id)).toEqual(['input', 'debug'])

    const file = await readFile(
      join(skillPipelineDir(targetRepo), 'fix-test-failure.json'),
      'utf8'
    )
    expect(JSON.parse(file).name).toBe('Fix test failure')

    await deleteSkillPipeline(targetRepo, 'fix-test-failure')
    expect(await listSkillPipelines(targetRepo)).toEqual([])
  })

  test('rejects edges that point at missing ports', () => {
    const pipeline = samplePipeline()
    pipeline.edges[0] = {
      id: 'bad-edge',
      from: { nodeId: 'input', portId: 'missing' },
      to: { nodeId: 'debug', portId: 'issue' }
    }

    const result = validateSkillPipeline(pipeline)

    expect(result.ok).toBe(false)
    expect(result.errors.join('\n')).toContain('missing output port')
  })
})
