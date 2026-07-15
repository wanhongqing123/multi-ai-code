import { formatRemoteImControlCommandHelp } from './controlCommands.js'
import type { RemoteImControlCommandName } from './controlCommands.js'
import type { RemoteImAicliOutputSourceKind } from './outputSanitizer.js'

export type RemoteImAicliControlMode = 'plan' | 'build'

export interface RemoteImControlSessionInfo {
  sessionId: string
  targetRepo: string
  command: string
  startedAtMs: number
}

export interface RemoteImSwitchAicliModeRequest {
  sessionId: string
  sourceKind: Extract<RemoteImAicliOutputSourceKind, 'codex' | 'opencode'>
  mode: RemoteImAicliControlMode
}

export type RemoteImSwitchAicliMode = (
  request: RemoteImSwitchAicliModeRequest
) => Promise<{ ok: true; text?: string } | { ok: false; error: string; text?: string }>

export interface RemoteImExecuteAicliCommandRequest {
  sessionId: string
  sourceKind: Extract<RemoteImAicliOutputSourceKind, 'codex' | 'opencode'>
  command: 'status' | 'model' | 'goal' | 'btw'
  model?: string
  reasoning?: string
  goal?: string
  task?: string
}

export type RemoteImExecuteAicliCommand = (
  request: RemoteImExecuteAicliCommandRequest
) => Promise<{ ok: true; text: string } | { ok: false; error: string; text?: string }>

export interface ExecuteRemoteImControlCommandInput {
  command: RemoteImControlCommandName
  args?: string
  session: RemoteImControlSessionInfo | null
  sourceKind: RemoteImAicliOutputSourceKind
  switchMode?: RemoteImSwitchAicliMode
  executeCommand?: RemoteImExecuteAicliCommand
  now?: () => number
}

export interface ExecuteRemoteImControlCommandResult {
  ok: boolean
  text: string
}

function displaySourceKind(sourceKind: RemoteImAicliOutputSourceKind): string {
  if (sourceKind === 'codex') return 'Codex'
  if (sourceKind === 'opencode') return 'OpenCode'
  if (sourceKind === 'claude') return 'Claude'
  return '未知 AICLI'
}

function displayMode(mode: RemoteImAicliControlMode): string {
  return mode === 'plan' ? '计划模式' : '执行模式'
}

function formatDuration(ms: number): string {
  const safeMs = Math.max(0, ms)
  const totalSeconds = Math.floor(safeMs / 1000)
  if (totalSeconds < 60) return `${totalSeconds} 秒`
  const totalMinutes = Math.floor(totalSeconds / 60)
  if (totalMinutes < 60) return `${totalMinutes} 分钟`
  const hours = Math.floor(totalMinutes / 60)
  const minutes = totalMinutes % 60
  return minutes > 0 ? `${hours} 小时 ${minutes} 分钟` : `${hours} 小时`
}

function formatStatus(
  session: RemoteImControlSessionInfo | null,
  sourceKind: RemoteImAicliOutputSourceKind,
  now: () => number
): string {
  if (!session) return '当前没有运行中的 AICLI。'

  return [
    `当前 AICLI：${displaySourceKind(sourceKind)}`,
    `会话：${session.sessionId}`,
    `目录：${session.targetRepo}`,
    `命令：${session.command}`,
    `运行时长：${formatDuration(now() - session.startedAtMs)}`
  ].join('\n')
}

function parseModelArgs(args?: string): { model?: string; reasoning?: string } {
  const trimmed = args?.trim() ?? ''
  if (!trimmed) return {}

  const reasoningMatch = /^(?:reasoning|reason|r|推理)\s+(.+)$/i.exec(trimmed)
  if (reasoningMatch?.[1]?.trim()) {
    return { reasoning: reasoningMatch[1].trim() }
  }

  return { model: trimmed }
}

async function switchMode(
  input: ExecuteRemoteImControlCommandInput,
  mode: RemoteImAicliControlMode
): Promise<ExecuteRemoteImControlCommandResult> {
  if (!input.session) {
    return {
      ok: false,
      text: '当前没有运行中的 AICLI，无法切换模式。'
    }
  }

  if (input.sourceKind === 'claude') {
    return {
      ok: false,
      text: 'Claude 暂不接入 IM 源码级控制命令。'
    }
  }

  if (input.sourceKind !== 'codex' && input.sourceKind !== 'opencode') {
    return {
      ok: false,
      text: '当前 AICLI 类型未知，无法安全切换模式。'
    }
  }

  if (!input.switchMode) {
    return {
      ok: false,
      text: `${displaySourceKind(input.sourceKind)} 源码级控制通道尚未接入，未向 TUI 注入命令。`
    }
  }

  const result = await input.switchMode({
    sessionId: input.session.sessionId,
    sourceKind: input.sourceKind,
    mode
  })
  if (!result.ok) {
    // 超时单独说清楚：命令已发出但没等到 AICLI 确认（可能任务繁忙或版本过旧），
    // 与“AICLI 明确拒绝”区分开，避免误导。
    if (result.error.includes('timed out')) {
      return {
        ok: false,
        text: `已发送切换到${displayMode(mode)}的命令，但未收到 AICLI 确认，请稍后用 /status 核实。`
      }
    }
    return {
      ok: false,
      text: `切换到${displayMode(mode)}失败：${result.text?.trim() || result.error}`
    }
  }

  return {
    ok: true,
    text: result.text?.trim() || `已切换到${displayMode(mode)}。`
  }
}

async function status(
  input: ExecuteRemoteImControlCommandInput
): Promise<ExecuteRemoteImControlCommandResult> {
  if (!input.session) {
    return {
      ok: false,
      text: '当前没有运行中的 AICLI。'
    }
  }

  if (input.sourceKind === 'codex' || input.sourceKind === 'opencode') {
    if (!input.executeCommand) {
      return {
        ok: false,
        text: `${displaySourceKind(input.sourceKind)} 源码级控制通道尚未接入，无法获取真实 /status 输出。`
      }
    }

    const result = await input.executeCommand({
      sessionId: input.session.sessionId,
      sourceKind: input.sourceKind,
      command: 'status'
    })
    if (!result.ok) {
      return {
        ok: false,
        text: result.text || `获取 ${displaySourceKind(input.sourceKind)} /status 失败：${result.error}`
      }
    }
    return {
      ok: true,
      text: result.text
    }
  }

  return {
    ok: true,
    text: formatStatus(input.session, input.sourceKind, input.now ?? (() => Date.now()))
  }
}

async function model(
  input: ExecuteRemoteImControlCommandInput,
  options: { forceList?: boolean } = {}
): Promise<ExecuteRemoteImControlCommandResult> {
  if (!input.session) {
    return {
      ok: false,
      text: '当前没有运行中的 AICLI，无法查看或切换模型。'
    }
  }

  if (input.sourceKind === 'claude') {
    return {
      ok: false,
      text: 'Claude 暂不接入 IM 源码级模型切换命令。'
    }
  }

  if (input.sourceKind !== 'codex' && input.sourceKind !== 'opencode') {
    return {
      ok: false,
      text: '当前 AICLI 类型未知，无法安全查看或切换模型。'
    }
  }

  if (!input.executeCommand) {
    return {
      ok: false,
      text: `${displaySourceKind(input.sourceKind)} 源码级控制通道尚未接入，无法执行 /model。`
    }
  }

  const result = await input.executeCommand({
    sessionId: input.session.sessionId,
    sourceKind: input.sourceKind,
    command: 'model',
    ...(options.forceList ? {} : parseModelArgs(input.args))
  })
  if (!result.ok) {
    return {
      ok: false,
      text: result.text || `执行 ${displaySourceKind(input.sourceKind)} /model 失败：${result.error}`
    }
  }
  return {
    ok: true,
    text: result.text
  }
}

async function btw(
  input: ExecuteRemoteImControlCommandInput
): Promise<ExecuteRemoteImControlCommandResult> {
  if (!input.session) {
    return {
      ok: false,
      text: '当前没有运行中的 AICLI，无法执行 /btw。'
    }
  }

  if (input.sourceKind === 'claude') {
    return {
      ok: false,
      text: 'Claude 暂不接入 IM 源码级 /btw 控制命令。'
    }
  }

  if (input.sourceKind !== 'codex' && input.sourceKind !== 'opencode') {
    return {
      ok: false,
      text: '当前 AICLI 类型未知，无法安全执行 /btw。'
    }
  }

  const task = input.args?.trim()
  if (!task) {
    return {
      ok: false,
      text: '用法：/btw <任务>'
    }
  }

  if (!input.executeCommand) {
    return {
      ok: false,
      text: `${displaySourceKind(input.sourceKind)} 源码级控制通道尚未接入，无法执行 /btw。`
    }
  }

  const result = await input.executeCommand({
    sessionId: input.session.sessionId,
    sourceKind: input.sourceKind,
    command: 'btw',
    task
  })
  if (!result.ok) {
    return {
      ok: false,
      text: result.text || `执行 ${displaySourceKind(input.sourceKind)} /btw 失败：${result.error}`
    }
  }
  return {
    ok: true,
    text: result.text
  }
}

async function goal(
  input: ExecuteRemoteImControlCommandInput
): Promise<ExecuteRemoteImControlCommandResult> {
  if (!input.session) {
    return {
      ok: false,
      text: '当前没有运行中的 AICLI，无法执行 /goal。'
    }
  }

  if (input.sourceKind === 'claude') {
    return {
      ok: false,
      text: 'Claude 暂不接入 IM 源码级 Goal 控制命令。'
    }
  }

  if (input.sourceKind !== 'codex' && input.sourceKind !== 'opencode') {
    return {
      ok: false,
      text: '当前 AICLI 类型未知，无法安全执行 /goal。'
    }
  }

  if (!input.executeCommand) {
    return {
      ok: false,
      text: `${displaySourceKind(input.sourceKind)} 源码级控制通道尚未接入，无法执行 /goal。`
    }
  }

  const result = await input.executeCommand({
    sessionId: input.session.sessionId,
    sourceKind: input.sourceKind,
    command: 'goal',
    goal: input.args?.trim() || undefined
  })
  if (!result.ok) {
    return {
      ok: false,
      text: result.text || `执行 ${displaySourceKind(input.sourceKind)} /goal 失败：${result.error}`
    }
  }
  return {
    ok: true,
    text: result.text
  }
}

export async function executeRemoteImControlCommand(
  input: ExecuteRemoteImControlCommandInput
): Promise<ExecuteRemoteImControlCommandResult> {
  if (input.command === 'help') {
    return { ok: true, text: formatRemoteImControlCommandHelp() }
  }

  if (input.command === 'status') {
    return status(input)
  }

  if (input.command === 'models') {
    return model(input, { forceList: true })
  }

  if (input.command === 'model') {
    return model(input)
  }

  if (input.command === 'goal') {
    return goal(input)
  }

  if (input.command === 'btw') {
    return btw(input)
  }

  if (input.command === 'plan') {
    return switchMode(input, 'plan')
  }

  return switchMode(input, 'build')
}
