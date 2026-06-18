import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { runSkill as runSkillFn, requiredVariables } from '../skillRunner'
import { localSkillToRunnableSkill, type LocalSkillSnapshot } from '../localSkillTypes'
import type { Skill } from '../skillTypes'
import {
  addSkillNode,
  connectPorts,
  createEmptyPipeline,
  getRunnableNodeOrder,
  markNodeStatus,
  moveNode,
  type SkillGraphEndpoint,
  type SkillGraphNode,
  type SkillGraphNodeStatus,
  type SkillGraphPort,
  type SkillGraphSkillSummary,
  type SkillPipeline
} from './skillGraphState'

interface SkillPipelineSummary {
  id: string
  name: string
  description: string | null
  nodeCount: number
  edgeCount: number
  updatedAt: string
}

interface Props {
  targetRepo: string | null
  sessionId: string | null
  sessionRunning: boolean
}

function normalizePipeline(pipeline: SkillPipeline): SkillPipeline {
  return {
    ...pipeline,
    description: pipeline.description ?? null,
    nodes: pipeline.nodes.map((node) => ({
      ...node,
      status: node.status ?? 'idle',
      error: node.error ?? null
    })),
    edges: pipeline.edges.map((edge) => ({ ...edge, status: edge.status ?? 'idle' }))
  }
}

function updateNode(
  pipeline: SkillPipeline,
  nodeId: string,
  patch: Partial<SkillGraphNode>
): SkillPipeline {
  return {
    ...pipeline,
    nodes: pipeline.nodes.map((node) =>
      node.id === nodeId ? { ...node, ...patch } : node
    ),
    updatedAt: new Date().toISOString()
  }
}

function updateNodePort(
  pipeline: SkillPipeline,
  nodeId: string,
  side: 'inputs' | 'outputs',
  portId: string,
  patch: Partial<SkillGraphPort>
): SkillPipeline {
  return {
    ...pipeline,
    nodes: pipeline.nodes.map((node) =>
      node.id === nodeId
        ? {
            ...node,
            [side]: node[side].map((port) =>
              port.id === portId ? { ...port, ...patch } : port
            )
          }
        : node
    ),
    updatedAt: new Date().toISOString()
  }
}

function addNodePort(
  pipeline: SkillPipeline,
  nodeId: string,
  side: 'inputs' | 'outputs'
): SkillPipeline {
  return {
    ...pipeline,
    nodes: pipeline.nodes.map((node) => {
      if (node.id !== nodeId) return node
      const prefix = side === 'inputs' ? 'input' : 'output'
      let index = node[side].length + 1
      const used = new Set(node[side].map((port) => port.id))
      while (used.has(`${prefix}_${index}`)) index++
      return {
        ...node,
        [side]: [
          ...node[side],
          {
            id: `${prefix}_${index}`,
            name: side === 'inputs' ? `Input ${index}` : `Output ${index}`,
            dataType: 'text'
          }
        ]
      }
    }),
    updatedAt: new Date().toISOString()
  }
}

function statusLabel(status: SkillGraphNodeStatus): string {
  switch (status) {
    case 'waiting':
      return '等待'
    case 'running':
      return '运行中'
    case 'success':
      return '成功'
    case 'failed':
      return '失败'
    case 'skipped':
      return '跳过'
    case 'cancelled':
      return '取消'
    default:
      return '未开始'
  }
}

function nodeCenter(node: SkillGraphNode): { x: number; y: number } {
  return { x: node.position.x + 90, y: node.position.y + 44 }
}

export default function SkillGraphPanel(props: Props): JSX.Element {
  const { targetRepo, sessionId, sessionRunning } = props
  const [skills, setSkills] = useState<Skill[]>([])
  const [pipelines, setPipelines] = useState<SkillPipelineSummary[]>([])
  const [pipeline, setPipeline] = useState<SkillPipeline>(() =>
    createEmptyPipeline('skill-pipeline', 'New skill pipeline')
  )
  const [selectedNodeId, setSelectedNodeId] = useState<string>('input')
  const [message, setMessage] = useState<string | null>(null)
  const [connectingFrom, setConnectingFrom] = useState<SkillGraphEndpoint | null>(null)
  const [draggingNodeId, setDraggingNodeId] = useState<string | null>(null)
  const [running, setRunning] = useState(false)
  const canvasRef = useRef<HTMLDivElement | null>(null)

  const selectedNode = pipeline.nodes.find((node) => node.id === selectedNodeId) ?? null
  const skillById = useMemo(() => new Map(skills.map((skill) => [skill.id, skill])), [skills])

  const refreshSkills = useCallback(async () => {
    const snapshot = (await window.api.habit.localSkills.scan()) as LocalSkillSnapshot
    setSkills(snapshot.skills.filter((skill) => skill.enabled).map(localSkillToRunnableSkill))
  }, [])

  const refreshPipelines = useCallback(async () => {
    if (!targetRepo) {
      setPipelines([])
      return
    }
    setPipelines(await window.api.habit.skillPipelines.list(targetRepo))
  }, [targetRepo])

  useEffect(() => {
    void refreshSkills()
  }, [refreshSkills])

  useEffect(() => {
    void refreshPipelines()
  }, [refreshPipelines])

  function flash(text: string): void {
    setMessage(text)
    setTimeout(() => setMessage(null), 2200)
  }

  async function loadPipeline(id: string): Promise<void> {
    if (!targetRepo) return
    const loaded = await window.api.habit.skillPipelines.read(targetRepo, id)
    if (!loaded) {
      flash('流水线不存在或读取失败')
      return
    }
    const next = normalizePipeline(loaded as SkillPipeline)
    setPipeline(next)
    setSelectedNodeId(next.nodes[0]?.id ?? 'input')
  }

  async function savePipeline(): Promise<void> {
    if (!targetRepo) {
      flash('请先选择项目')
      return
    }
    const result = await window.api.habit.skillPipelines.save(targetRepo, pipeline)
    if (!result.ok) {
      flash(result.errors?.join('; ') ?? result.error)
      return
    }
    const saved = normalizePipeline(result.pipeline as SkillPipeline)
    setPipeline(saved)
    await refreshPipelines()
    flash('已保存流水线')
  }

  async function deleteCurrentPipeline(): Promise<void> {
    if (!targetRepo || !confirm(`删除流水线 "${pipeline.name}"？`)) return
    await window.api.habit.skillPipelines.delete(targetRepo, pipeline.id)
    const next = createEmptyPipeline('skill-pipeline', 'New skill pipeline')
    setPipeline(next)
    setSelectedNodeId('input')
    await refreshPipelines()
  }

  function canvasPoint(event: React.DragEvent | React.PointerEvent): { x: number; y: number } {
    const rect = canvasRef.current?.getBoundingClientRect()
    if (!rect) return { x: 80, y: 80 }
    return {
      x: Math.max(20, event.clientX - rect.left),
      y: Math.max(20, event.clientY - rect.top)
    }
  }

  function onDropSkill(event: React.DragEvent<HTMLDivElement>): void {
    event.preventDefault()
    const skillId = event.dataTransfer.getData('application/x-skill-id')
    const skill = skills.find((item) => item.id === skillId)
    if (!skill) return
    const summary: SkillGraphSkillSummary = {
      id: String(skill.id),
      name: skill.name,
      description: skill.description
    }
    const next = addSkillNode(pipeline, summary, canvasPoint(event))
    setPipeline(next)
    setSelectedNodeId(next.nodes[next.nodes.length - 1].id)
  }

  function onNodePointerMove(event: React.PointerEvent<HTMLDivElement>): void {
    if (!draggingNodeId || event.buttons !== 1) return
    setPipeline((current) => moveNode(current, draggingNodeId, canvasPoint(event)))
  }

  function connectTo(to: SkillGraphEndpoint): void {
    if (!connectingFrom) return
    if (connectingFrom.nodeId === to.nodeId) {
      setConnectingFrom(null)
      return
    }
    setPipeline((current) => connectPorts(current, connectingFrom, to))
    setConnectingFrom(null)
  }

  async function runPipeline(): Promise<void> {
    if (!sessionId || !sessionRunning) {
      flash('请先启动主会话')
      return
    }
    setRunning(true)
    let current = {
      ...pipeline,
      nodes: pipeline.nodes.map((node) => ({
        ...node,
        status: node.kind === 'skill' ? 'waiting' : node.status
      }))
    } as SkillPipeline
    setPipeline(current)
    const outputs = new Map<string, string>()
    outputs.set('input.user_request', pipeline.description || pipeline.name)

    try {
      for (const node of getRunnableNodeOrder(current)) {
        const skill = node.skillId ? skillById.get(node.skillId) : null
        if (!skill) {
          current = markNodeStatus(current, node.id, 'failed', 'Skill 不存在或已禁用')
          setPipeline(current)
          break
        }
        current = markNodeStatus(current, node.id, 'running')
        setPipeline(current)

        const vars: Record<string, string> = {}
        for (const name of requiredVariables(skill)) {
          vars[name] = pipeline.description || pipeline.name
        }
        for (const edge of current.edges.filter((item) => item.to.nodeId === node.id)) {
          const value = outputs.get(`${edge.from.nodeId}.${edge.from.portId}`)
          if (!value) continue
          const targetPort = node.inputs.find((port) => port.id === edge.to.portId)
          if (targetPort) {
            vars[targetPort.id] = value
            vars[targetPort.name] = value
          }
        }

        const outcome = await runSkillFn(
          {
            sendUser: (sid, text) => window.api.cc.sendUser(sid, text),
            onData: window.api.cc.onData,
            touchLastUsed: (id) =>
              typeof id === 'number'
                ? window.api.habit.skills.touchLastUsed(id).then(() => undefined)
                : undefined
          },
          { sessionId, skill, vars, captureResponse: true }
        )

        if (!outcome.ok) {
          current = markNodeStatus(
            current,
            node.id,
            'failed',
            outcome.failedReason ?? '运行失败'
          )
          setPipeline(current)
          break
        }

        const captured = outcome.capturedResponse?.trim() || `${node.title} completed`
        for (const port of node.outputs) {
          outputs.set(`${node.id}.${port.id}`, captured)
        }
        current = markNodeStatus(current, node.id, 'success')
        setPipeline(current)
      }
    } finally {
      setRunning(false)
    }
  }

  return (
    <div className="skill-graph-panel">
      <div className="skill-graph-toolbar">
        <label>
          流水线
          <select
            className="plan-name-input"
            value={pipeline.id}
            onChange={(event) => void loadPipeline(event.target.value)}
          >
            <option value={pipeline.id}>{pipeline.name}</option>
            {pipelines
              .filter((item) => item.id !== pipeline.id)
              .map((item) => (
                <option key={item.id} value={item.id}>
                  {item.name}
                </option>
              ))}
          </select>
        </label>
        <input
          className="plan-name-input"
          value={pipeline.name}
          onChange={(event) =>
            setPipeline((current) => ({ ...current, name: event.target.value }))
          }
          placeholder="流水线名称"
        />
        <button
          type="button"
          className="drawer-btn"
          onClick={() => {
            const next = createEmptyPipeline(`skill-pipeline-${Date.now()}`, 'New skill pipeline')
            setPipeline(next)
            setSelectedNodeId('input')
          }}
        >
          新建
        </button>
        <button type="button" className="drawer-btn primary" onClick={() => void savePipeline()}>
          保存
        </button>
        <button type="button" className="drawer-btn" onClick={() => void runPipeline()} disabled={running}>
          {running ? '运行中' : '运行'}
        </button>
        <button type="button" className="drawer-btn warn" onClick={() => void deleteCurrentPipeline()}>
          删除
        </button>
        <button type="button" className="drawer-btn" onClick={() => void refreshSkills()}>
          刷新已启用 Skill
        </button>
        {message && <span className="habit-toast-inline">{message}</span>}
      </div>

      {!targetRepo ? (
        <div className="drawer-empty">请先选择项目，Skill Pipeline 会保存到当前项目仓库。</div>
      ) : (
        <div className="skill-graph-layout">
          <aside className="skill-graph-sidebar">
            <strong>已启用本机 Skills</strong>
            <div className="habit-settings-hint">只显示在 Skill 管理中启用的 Skill；拖到画布中创建节点。</div>
            <ul className="skill-graph-skill-list">
              {skills.map((skill) => (
                <li
                  key={skill.id}
                  draggable
                  onDragStart={(event) =>
                    event.dataTransfer.setData('application/x-skill-id', String(skill.id))
                  }
                  className="skill-graph-skill-item"
                >
                  <strong>{skill.name}</strong>
                  {skill.description && <span>{skill.description}</span>}
                </li>
              ))}
            </ul>
          </aside>

          <div
            ref={canvasRef}
            className="skill-graph-canvas"
            onDragOver={(event) => event.preventDefault()}
            onDrop={onDropSkill}
            onPointerMove={onNodePointerMove}
            onPointerUp={() => setDraggingNodeId(null)}
          >
            <svg className="skill-graph-edges">
              {pipeline.edges.map((edge) => {
                const from = pipeline.nodes.find((node) => node.id === edge.from.nodeId)
                const to = pipeline.nodes.find((node) => node.id === edge.to.nodeId)
                if (!from || !to) return null
                const a = nodeCenter(from)
                const b = nodeCenter(to)
                return (
                  <line
                    key={edge.id}
                    x1={a.x}
                    y1={a.y}
                    x2={b.x}
                    y2={b.y}
                    className={`skill-graph-edge skill-graph-edge-${edge.status}`}
                  />
                )
              })}
            </svg>

            {pipeline.nodes.map((node) => (
              <div
                key={node.id}
                className={`skill-graph-node skill-graph-node-${node.status} ${selectedNodeId === node.id ? 'selected' : ''}`}
                style={{ left: node.position.x, top: node.position.y }}
                onPointerDown={(event) => {
                  if ((event.target as HTMLElement).classList.contains('skill-graph-port')) {
                    return
                  }
                  setDraggingNodeId(node.id)
                  setSelectedNodeId(node.id)
                }}
              >
                <div className="skill-graph-node-head">
                  <strong>{node.title}</strong>
                  <span>{statusLabel(node.status)}</span>
                </div>
                <div className="skill-graph-node-ports">
                  <div>
                    {node.inputs.map((port) => (
                      <button
                        key={port.id}
                        type="button"
                        className="skill-graph-port input"
                        onPointerUp={() => connectTo({ nodeId: node.id, portId: port.id })}
                      >
                        ◀ {port.name}
                      </button>
                    ))}
                  </div>
                  <div>
                    {node.outputs.map((port) => (
                      <button
                        key={port.id}
                        type="button"
                        className="skill-graph-port output"
                        onPointerDown={(event) => {
                          event.stopPropagation()
                          setConnectingFrom({ nodeId: node.id, portId: port.id })
                        }}
                      >
                        {port.name} ▶
                      </button>
                    ))}
                  </div>
                </div>
                {node.error && <div className="skill-graph-node-error">{node.error}</div>}
              </div>
            ))}
          </div>

          <aside className="skill-graph-config">
            <strong>节点配置</strong>
            {!selectedNode ? (
              <div className="drawer-empty">选择一个节点进行配置。</div>
            ) : (
              <>
                <label>
                  标题
                  <input
                    className="plan-name-input"
                    value={selectedNode.title}
                    onChange={(event) =>
                      setPipeline((current) =>
                        updateNode(current, selectedNode.id, { title: event.target.value })
                      )
                    }
                  />
                </label>
                <label>
                  说明
                  <textarea
                    className="habit-step-text"
                    rows={3}
                    value={selectedNode.description ?? ''}
                    onChange={(event) =>
                      setPipeline((current) =>
                        updateNode(current, selectedNode.id, {
                          description: event.target.value
                        })
                      )
                    }
                  />
                </label>
                <section>
                  <div className="skill-graph-config-head">
                    <strong>输入端口</strong>
                    <button
                      type="button"
                      className="drawer-btn"
                      onClick={() =>
                        setPipeline((current) => addNodePort(current, selectedNode.id, 'inputs'))
                      }
                    >
                      添加
                    </button>
                  </div>
                  {selectedNode.inputs.map((port) => (
                    <input
                      key={port.id}
                      className="plan-name-input"
                      value={port.name}
                      onChange={(event) =>
                        setPipeline((current) =>
                          updateNodePort(current, selectedNode.id, 'inputs', port.id, {
                            name: event.target.value
                          })
                        )
                      }
                    />
                  ))}
                </section>
                <section>
                  <div className="skill-graph-config-head">
                    <strong>输出端口</strong>
                    <button
                      type="button"
                      className="drawer-btn"
                      onClick={() =>
                        setPipeline((current) => addNodePort(current, selectedNode.id, 'outputs'))
                      }
                    >
                      添加
                    </button>
                  </div>
                  {selectedNode.outputs.map((port) => (
                    <input
                      key={port.id}
                      className="plan-name-input"
                      value={port.name}
                      onChange={(event) =>
                        setPipeline((current) =>
                          updateNodePort(current, selectedNode.id, 'outputs', port.id, {
                            name: event.target.value
                          })
                        )
                      }
                    />
                  ))}
                </section>
                <section>
                  <strong>连接</strong>
                  <ul className="skill-graph-edge-list">
                    {pipeline.edges.map((edge) => (
                      <li key={edge.id}>
                        {edge.from.nodeId}.{edge.from.portId} → {edge.to.nodeId}.{edge.to.portId}
                      </li>
                    ))}
                  </ul>
                </section>
              </>
            )}
          </aside>
        </div>
      )}
    </div>
  )
}
