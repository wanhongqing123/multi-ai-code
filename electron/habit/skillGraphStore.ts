import { promises as fs } from 'fs'
import { join } from 'path'
import { joinWithRootStyle } from '../pathStyle.js'

export type SkillGraphDataType = 'text' | 'json' | 'artifact'
export type SkillGraphNodeKind = 'input' | 'skill' | 'output'
export type SkillGraphNodeStatus =
  | 'idle'
  | 'waiting'
  | 'running'
  | 'success'
  | 'failed'
  | 'skipped'
  | 'cancelled'
export type SkillGraphEdgeStatus = 'idle' | 'ready' | 'passing' | 'success' | 'failed'

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
  status?: SkillGraphNodeStatus
  error?: string | null
}

export interface SkillGraphEdgeEndpoint {
  nodeId: string
  portId: string
}

export interface SkillGraphEdge {
  id: string
  from: SkillGraphEdgeEndpoint
  to: SkillGraphEdgeEndpoint
  status?: SkillGraphEdgeStatus
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

export interface SkillPipelineSummary {
  id: string
  name: string
  description: string | null
  nodeCount: number
  edgeCount: number
  updatedAt: string
}

export type SkillPipelineValidationResult =
  | { ok: true; errors: [] }
  | { ok: false; errors: string[] }

export type SaveSkillPipelineResult =
  | { ok: true; pipeline: SkillPipeline }
  | { ok: false; error: string; errors?: string[] }

export function skillPipelineDir(targetRepo: string): string {
  return joinWithRootStyle(targetRepo, '.multi-ai-code', 'skill-pipelines')
}

function pipelinePath(targetRepo: string, id: string): string {
  return join(skillPipelineDir(targetRepo), `${sanitizePipelineId(id)}.json`)
}

export function sanitizePipelineId(value: string): string {
  return value
    .trim()
    .toLowerCase()
    .replace(/[^a-z0-9_-]+/g, '-')
    .replace(/^-+|-+$/g, '')
}

function hasPort(
  nodes: Map<string, SkillGraphNode>,
  endpoint: SkillGraphEdgeEndpoint,
  side: 'input' | 'output'
): boolean {
  const node = nodes.get(endpoint.nodeId)
  if (!node) return false
  const ports = side === 'input' ? node.inputs : node.outputs
  return ports.some((port) => port.id === endpoint.portId)
}

export function validateSkillPipeline(
  pipeline: SkillPipeline
): SkillPipelineValidationResult {
  const errors: string[] = []
  if (!sanitizePipelineId(pipeline.id)) errors.push('pipeline id is required')
  if (!pipeline.name.trim()) errors.push('pipeline name is required')

  const nodes = new Map<string, SkillGraphNode>()
  for (const node of pipeline.nodes) {
    if (!node.id.trim()) {
      errors.push('node id is required')
      continue
    }
    if (nodes.has(node.id)) errors.push(`duplicate node id: ${node.id}`)
    nodes.set(node.id, node)
    if (node.kind === 'skill' && typeof node.skillId !== 'string') {
      errors.push(`skill node ${node.id} is missing skillId`)
    }
  }

  for (const edge of pipeline.edges) {
    if (!nodes.has(edge.from.nodeId)) {
      errors.push(`edge ${edge.id} references missing source node ${edge.from.nodeId}`)
      continue
    }
    if (!nodes.has(edge.to.nodeId)) {
      errors.push(`edge ${edge.id} references missing target node ${edge.to.nodeId}`)
      continue
    }
    if (!hasPort(nodes, edge.from, 'output')) {
      errors.push(`edge ${edge.id} references missing output port ${edge.from.nodeId}.${edge.from.portId}`)
    }
    if (!hasPort(nodes, edge.to, 'input')) {
      errors.push(`edge ${edge.id} references missing input port ${edge.to.nodeId}.${edge.to.portId}`)
    }
  }

  return errors.length === 0 ? { ok: true, errors: [] } : { ok: false, errors }
}

function summarizePipeline(pipeline: SkillPipeline): SkillPipelineSummary {
  return {
    id: pipeline.id,
    name: pipeline.name,
    description: pipeline.description ?? null,
    nodeCount: pipeline.nodes.length,
    edgeCount: pipeline.edges.length,
    updatedAt: pipeline.updatedAt
  }
}

async function readPipelineFile(filePath: string): Promise<SkillPipeline | null> {
  try {
    const raw = await fs.readFile(filePath, 'utf8')
    return JSON.parse(raw) as SkillPipeline
  } catch {
    return null
  }
}

export async function listSkillPipelines(
  targetRepo: string
): Promise<SkillPipelineSummary[]> {
  let files: string[]
  try {
    files = await fs.readdir(skillPipelineDir(targetRepo))
  } catch {
    return []
  }

  const pipelines = await Promise.all(
    files
      .filter((file) => file.endsWith('.json'))
      .map((file) => readPipelineFile(join(skillPipelineDir(targetRepo), file)))
  )
  return pipelines
    .filter((pipeline): pipeline is SkillPipeline => pipeline !== null)
    .map(summarizePipeline)
    .sort((left, right) => right.updatedAt.localeCompare(left.updatedAt))
}

export async function readSkillPipeline(
  targetRepo: string,
  id: string
): Promise<SkillPipeline | null> {
  return readPipelineFile(pipelinePath(targetRepo, id))
}

export async function saveSkillPipeline(
  targetRepo: string,
  pipeline: SkillPipeline
): Promise<SaveSkillPipelineResult> {
  const cleanId = sanitizePipelineId(pipeline.id || pipeline.name)
  if (!cleanId) return { ok: false, error: 'pipeline id is required' }

  const now = new Date().toISOString()
  const next: SkillPipeline = {
    ...pipeline,
    id: cleanId,
    description: pipeline.description ?? null,
    createdAt: pipeline.createdAt || now,
    updatedAt: now
  }
  const validation = validateSkillPipeline(next)
  if (!validation.ok) {
    return { ok: false, error: 'invalid skill pipeline', errors: validation.errors }
  }

  await fs.mkdir(skillPipelineDir(targetRepo), { recursive: true })
  await fs.writeFile(pipelinePath(targetRepo, cleanId), JSON.stringify(next, null, 2), 'utf8')
  return { ok: true, pipeline: next }
}

export async function deleteSkillPipeline(targetRepo: string, id: string): Promise<void> {
  await fs.rm(pipelinePath(targetRepo, id), { force: true })
}
