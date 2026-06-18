import { describe, expect, test } from 'vitest'
import {
  addSkillNode,
  connectPorts,
  createEmptyPipeline,
  getRunnableNodeOrder,
  markNodeStatus,
  type SkillGraphSkillSummary
} from './skillGraphState'

const debugSkill: SkillGraphSkillSummary = {
  id: 'skill-debugging',
  name: 'systematic-debugging',
  description: 'Debug failures'
}

const tddSkill: SkillGraphSkillSummary = {
  id: 'skill-tdd',
  name: 'test-driven-development',
  description: 'Write tests first'
}

describe('skill graph state helpers', () => {
  test('adds draggable skill nodes with default input and output ports', () => {
    const pipeline = createEmptyPipeline('fix-test-failure', 'Fix test failure')
    const next = addSkillNode(pipeline, debugSkill, { x: 120, y: 80 })

    expect(next.nodes).toHaveLength(2)
    expect(next.nodes[1]).toMatchObject({
      kind: 'skill',
      skillId: 'skill-debugging',
      title: 'systematic-debugging',
      position: { x: 120, y: 80 },
      inputs: [{ id: 'input', name: 'Input', dataType: 'text' }],
      outputs: [{ id: 'output', name: 'Output', dataType: 'text' }]
    })
  })

  test('connects output ports to input ports and computes runnable order', () => {
    let pipeline = createEmptyPipeline('fix-test-failure', 'Fix test failure')
    pipeline = addSkillNode(pipeline, debugSkill, { x: 120, y: 80 })
    pipeline = addSkillNode(pipeline, tddSkill, { x: 360, y: 80 })
    pipeline = connectPorts(
      pipeline,
      { nodeId: 'node-1', portId: 'output' },
      { nodeId: 'node-2', portId: 'input' }
    )

    expect(pipeline.edges).toHaveLength(1)
    expect(getRunnableNodeOrder(pipeline).map((node) => node.id)).toEqual([
      'node-1',
      'node-2'
    ])
  })

  test('updates node status without mutating the previous pipeline object', () => {
    const pipeline = addSkillNode(
      createEmptyPipeline('fix-test-failure', 'Fix test failure'),
      debugSkill,
      { x: 120, y: 80 }
    )

    const next = markNodeStatus(pipeline, 'node-1', 'running')

    expect(pipeline.nodes[1].status).toBe('idle')
    expect(next.nodes[1].status).toBe('running')
  })
})
