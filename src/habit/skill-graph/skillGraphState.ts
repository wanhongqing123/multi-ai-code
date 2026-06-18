export type SkillGraphDataType = 'text' | 'json' | 'artifact'
export type SkillGraphNodeStatus =
  | 'idle'
  | 'waiting'
  | 'running'
  | 'success'
  | 'failed'
  | 'skipped'
  | 'cancelled'
export type SkillGraphNodeKind = 'input' | 'skill' | 'output'

export interface SkillGraphPort {
  id: string
  name: string
  dataType: SkillGraphDataType
}

export interface SkillGraphNode {
  id: string
  kind: SkillGraphNodeKind
  title: string
  skillId?: string
  description?: string | null
  position: { x: number; y: number }
  inputs: SkillGraphPort[]
  outputs: SkillGraphPort[]
  status: SkillGraphNodeStatus
  error?: string | null
}

export interface SkillGraphEndpoint {
  nodeId: string
  portId: string
}

export interface SkillGraphEdge {
  id: string
  from: SkillGraphEndpoint
  to: SkillGraphEndpoint
  status: 'idle' | 'ready' | 'passing' | 'success' | 'failed'
}

export interface SkillPipeline {
  id: string
  name: string
  description?: string | null
  nodes: SkillGraphNode[]
  edges: SkillGraphEdge[]
  createdAt: string
  updatedAt: string
}

export interface SkillGraphSkillSummary {
  id: string
  name: string
  description: string | null
}

export function createEmptyPipeline(id: string, name: string): SkillPipeline {
  const now = new Date().toISOString()
  return {
    id,
    name,
    description: null,
    nodes: [
      {
        id: 'input',
        kind: 'input',
        title: 'Input',
        position: { x: 40, y: 120 },
        inputs: [],
        outputs: [{ id: 'user_request', name: 'User request', dataType: 'text' }],
        status: 'idle'
      }
    ],
    edges: [],
    createdAt: now,
    updatedAt: now
  }
}

function nextNodeId(pipeline: SkillPipeline): string {
  let index = 1
  const used = new Set(pipeline.nodes.map((node) => node.id))
  while (used.has(`node-${index}`)) index++
  return `node-${index}`
}

export function addSkillNode(
  pipeline: SkillPipeline,
  skill: SkillGraphSkillSummary,
  position: { x: number; y: number }
): SkillPipeline {
  return {
    ...pipeline,
    nodes: [
      ...pipeline.nodes,
      {
        id: nextNodeId(pipeline),
        kind: 'skill',
        skillId: skill.id,
        title: skill.name,
        description: skill.description,
        position,
        inputs: [{ id: 'input', name: 'Input', dataType: 'text' }],
        outputs: [{ id: 'output', name: 'Output', dataType: 'text' }],
        status: 'idle'
      }
    ],
    updatedAt: new Date().toISOString()
  }
}

export function connectPorts(
  pipeline: SkillPipeline,
  from: SkillGraphEndpoint,
  to: SkillGraphEndpoint
): SkillPipeline {
  const edge: SkillGraphEdge = {
    id: `edge-${pipeline.edges.length + 1}`,
    from,
    to,
    status: 'idle'
  }
  return {
    ...pipeline,
    edges: [...pipeline.edges, edge],
    updatedAt: new Date().toISOString()
  }
}

export function markNodeStatus(
  pipeline: SkillPipeline,
  nodeId: string,
  status: SkillGraphNodeStatus,
  error: string | null = null
): SkillPipeline {
  return {
    ...pipeline,
    nodes: pipeline.nodes.map((node) =>
      node.id === nodeId ? { ...node, status, error } : node
    ),
    updatedAt: new Date().toISOString()
  }
}

export function moveNode(
  pipeline: SkillPipeline,
  nodeId: string,
  position: { x: number; y: number }
): SkillPipeline {
  return {
    ...pipeline,
    nodes: pipeline.nodes.map((node) =>
      node.id === nodeId ? { ...node, position } : node
    ),
    updatedAt: new Date().toISOString()
  }
}

export function getRunnableNodeOrder(pipeline: SkillPipeline): SkillGraphNode[] {
  const skillNodes = pipeline.nodes.filter((node) => node.kind === 'skill')
  const nodeById = new Map(skillNodes.map((node) => [node.id, node]))
  const indegree = new Map(skillNodes.map((node) => [node.id, 0]))
  const outgoing = new Map<string, string[]>()

  for (const edge of pipeline.edges) {
    if (!nodeById.has(edge.from.nodeId) || !nodeById.has(edge.to.nodeId)) continue
    outgoing.set(edge.from.nodeId, [...(outgoing.get(edge.from.nodeId) ?? []), edge.to.nodeId])
    indegree.set(edge.to.nodeId, (indegree.get(edge.to.nodeId) ?? 0) + 1)
  }

  const queue = skillNodes.filter((node) => (indegree.get(node.id) ?? 0) === 0)
  const ordered: SkillGraphNode[] = []
  while (queue.length > 0) {
    const node = queue.shift()!
    ordered.push(node)
    for (const nextId of outgoing.get(node.id) ?? []) {
      const nextDegree = (indegree.get(nextId) ?? 0) - 1
      indegree.set(nextId, nextDegree)
      if (nextDegree === 0) {
        const next = nodeById.get(nextId)
        if (next) queue.push(next)
      }
    }
  }

  return ordered.length === skillNodes.length ? ordered : skillNodes
}
