import { createHash, randomUUID } from 'node:crypto'
import type {
  RemoteImApprovalAction,
  RemoteImTextInteraction,
  RemoteImApprovalResolutionOutcome
} from './types.js'

export type RemoteImApprovalDecision = 'accept' | 'accept-persistent' | 'cancel'

export interface RemoteImApprovalRequest {
  projectId: string
  requesterUserId: string
  sessionId: string
  taskId: string
  replyId: string
  threadId: string
  turnId: string
  approvalId: string
  commandText: string
  cwd: string
  reason?: string
  persistentApprovalCommand?: string
}

export interface RemoteImApprovalResolution extends RemoteImApprovalRequest {
  decision: RemoteImApprovalDecision
}

export interface RemoteImApprovalDecisionInput {
  projectId: string
  fromUserId: string
  token: string
  action: RemoteImApprovalAction
}

export interface RemoteImApprovalDecisionResult {
  handled: boolean
  ok: boolean
  text: string
}

export interface RemoteImApprovalCoordinatorDeps {
  sendText(
    projectId: string,
    toUserId: string,
    text: string,
    interaction?: RemoteImTextInteraction
  ): Promise<{ ok: boolean; error?: string }>
  resolveApproval(
    input: RemoteImApprovalResolution
  ): Promise<{
    ok: boolean
    error?: string
    text?: string
    approvalResolution?: {
      applied: boolean
      winnerDecision: string
      winnerSource: string
    }
  }>
  now?: () => number
  createToken?: () => string
  setTimer?: (callback: () => void, timeoutMs: number) => ReturnType<typeof setTimeout>
  clearTimer?: (timer: ReturnType<typeof setTimeout>) => void
  timeoutMs?: number
  maxEntriesPerSession?: number
  getSecurityGeneration?: () => number
  isSecurityContextCurrent?: () => boolean
}

type RemoteImApprovalState =
  | 'pending'
  | 'resolving'
  | 'approved'
  | 'rejected'
  | 'expired'
  | 'session-cancelled'
  | 'failed'
  | 'resolved-awaiting-control-result'
  | 'resolved'

interface PendingRemoteImApproval extends RemoteImApprovalRequest {
  token: string
  commandHash: string
  createdAt: number
  expiresAt: number
  state: RemoteImApprovalState
  timer: ReturnType<typeof setTimeout> | null
  securityGeneration: number
  resolutionNoticeSent: boolean
}

const DEFAULT_APPROVAL_TIMEOUT_MS = 10 * 60 * 1000
const MAX_TERMINAL_APPROVAL_ENTRIES = 4096
const INVALID_APPROVAL_TEXT = '审批指令无效、已处理或已过期。'

function cleanRequired(value: string): string {
  return value.trim()
}

function approvalIdentity(input: Pick<RemoteImApprovalRequest, 'sessionId' | 'taskId' | 'replyId' | 'threadId' | 'turnId' | 'approvalId'>): string {
  return JSON.stringify([
    input.sessionId,
    input.taskId,
    input.replyId,
    input.threadId,
    input.turnId,
    input.approvalId
  ])
}

function commandHash(
  commandText: string,
  cwd: string,
  persistentApprovalCommand?: string
): string {
  return createHash('sha256')
    .update(JSON.stringify([commandText, cwd, persistentApprovalCommand ?? null]))
    .digest('hex')
}

function escapeApprovalDisplayControls(commandText: string): string {
  return commandText.replace(
    /[\u0000-\u0009\u000b-\u001f\u007f-\u009f\u00ad\u034f\u061c\u180e\u200b-\u200f\u2028-\u202e\u2060-\u206f\ufeff]/gu,
    (character) => `\\u{${character.codePointAt(0)?.toString(16).padStart(4, '0')}}`
  )
}

function formatCommandBlock(commandText: string): string {
  // Four-space indentation keeps command contents inert even if they contain
  // Markdown fences or text that looks like an approval command. Escape
  // terminal controls and bidi overrides so the visible command cannot spoof
  // the surrounding confirmation UI; newlines remain visible and indented.
  return escapeApprovalDisplayControls(commandText)
    .split('\n')
    .map((line) => `    ${line}`)
    .join('\n')
}

function formatApprovalRequest(item: PendingRemoteImApproval): string {
  const minutes = Math.max(1, Math.ceil((item.expiresAt - item.createdAt) / 60_000))
  const lines = [
    'Codex 请求执行一条高风险命令：',
    '',
    formatCommandBlock(item.commandText),
    '',
    '工作目录：',
    formatCommandBlock(item.cwd)
  ]
  if (item.reason) {
    lines.push('申请原因：', formatCommandBlock(item.reason))
  }
  lines.push(
    '',
    `请在 ${minutes} 分钟内由本消息对应的请求人确认。`,
    '请使用 MaiChat 消息卡片下方的按钮选择本次审批结果。'
  )
  if (item.persistentApprovalCommand) {
    lines.push(
      '',
      '“同意并记住”将记住以下命令前缀，后续匹配的命令不再询问：',
      '',
      formatCommandBlock(item.persistentApprovalCommand)
    )
  }
  return lines.join('\n')
}

export class RemoteImApprovalCoordinator {
  private readonly byToken = new Map<string, PendingRemoteImApproval>()
  private readonly tokenByIdentity = new Map<string, string>()
  private readonly timeoutMs: number
  private readonly now: () => number
  private readonly createToken: () => string
  private readonly setTimer: NonNullable<RemoteImApprovalCoordinatorDeps['setTimer']>
  private readonly clearTimer: NonNullable<RemoteImApprovalCoordinatorDeps['clearTimer']>
  private readonly maxEntriesPerSession: number
  private readonly getSecurityGeneration: () => number
  private readonly isSecurityContextCurrent: () => boolean

  constructor(private readonly deps: RemoteImApprovalCoordinatorDeps) {
    this.timeoutMs = deps.timeoutMs ?? DEFAULT_APPROVAL_TIMEOUT_MS
    this.now = deps.now ?? Date.now
    this.createToken = deps.createToken ?? (() => `approval-${randomUUID()}`)
    this.setTimer = deps.setTimer ?? setTimeout
    this.clearTimer = deps.clearTimer ?? clearTimeout
    this.maxEntriesPerSession = deps.maxEntriesPerSession ?? 2048
    this.getSecurityGeneration = deps.getSecurityGeneration ?? (() => 0)
    this.isSecurityContextCurrent = deps.isSecurityContextCurrent ?? (() => true)
  }

  async register(input: RemoteImApprovalRequest): Promise<{ ok: boolean; token?: string; error?: string }> {
    const request: RemoteImApprovalRequest = {
      projectId: cleanRequired(input.projectId),
      requesterUserId: cleanRequired(input.requesterUserId),
      sessionId: cleanRequired(input.sessionId),
      taskId: cleanRequired(input.taskId),
      replyId: cleanRequired(input.replyId),
      threadId: cleanRequired(input.threadId),
      turnId: cleanRequired(input.turnId),
      approvalId: cleanRequired(input.approvalId),
      commandText: input.commandText,
      // Command context is security-sensitive evidence shown to the approver.
      // Use trimming only for validation; preserve the exact bridge payload for
      // display and for the correlated resolution request.
      cwd: input.cwd,
      ...(input.reason !== undefined && input.reason.trim()
        ? { reason: input.reason }
        : {}),
      ...(input.persistentApprovalCommand !== undefined &&
        input.persistentApprovalCommand.trim()
        ? { persistentApprovalCommand: input.persistentApprovalCommand }
        : {})
    }
    if (
      !request.projectId ||
      !request.requesterUserId ||
      !request.sessionId ||
      !request.taskId ||
      !request.replyId ||
      !request.threadId ||
      !request.turnId ||
      !request.approvalId ||
      !request.commandText.trim() ||
      !request.cwd.trim()
    ) {
      return { ok: false, error: 'approval request is missing correlation fields' }
    }
    if (!this.isSecurityContextCurrent()) {
      await this.cancelRequestFailClosed(request)
      return { ok: false, error: 'approval authority is changing' }
    }

    const identity = approvalIdentity(request)
    const previousToken = this.tokenByIdentity.get(identity)
    if (previousToken) {
      const previous = this.byToken.get(previousToken)
      if (previous) {
        // Reliable bridge delivery may repeat the same event until it receives
        // an ack. Never create or send a second approval capability for it.
        if (
          previous.projectId === request.projectId &&
          previous.requesterUserId === request.requesterUserId &&
          previous.commandHash ===
            commandHash(
              request.commandText,
              request.cwd,
              request.persistentApprovalCommand
            )
        ) {
          return previous.state === 'pending' || previous.state === 'resolving'
            ? { ok: true, token: previous.token }
            : { ok: false, token: previous.token, error: 'approval was already resolved' }
        }
        await this.cancelRequestFailClosed(request)
        return { ok: false, error: 'approval identity collision' }
      }
    }

    if (
      [...this.byToken.values()].filter(
        (item) =>
          item.sessionId === request.sessionId &&
          (item.state === 'pending' || item.state === 'resolving')
      ).length >=
      this.maxEntriesPerSession
    ) {
      await this.cancelRequestFailClosed(request)
      return { ok: false, error: 'too many approval requests in this session' }
    }

    const createdAt = this.now()
    const token = this.createUniqueToken()
    if (!token) {
      await this.cancelRequestFailClosed(request)
      return { ok: false, error: 'failed to allocate a unique approval token' }
    }
    const item: PendingRemoteImApproval = {
      ...request,
      token,
      commandHash: commandHash(
        request.commandText,
        request.cwd,
        request.persistentApprovalCommand
      ),
      createdAt,
      expiresAt: createdAt + this.timeoutMs,
      state: 'pending',
      timer: null,
      securityGeneration: this.getSecurityGeneration(),
      resolutionNoticeSent: false
    }
    item.timer = this.setTimer(() => {
      void this.expire(item)
    }, this.timeoutMs)
    this.byToken.set(token, item)
    this.tokenByIdentity.set(identity, token)

    let sent: { ok: boolean; error?: string }
    try {
      sent = await this.deps.sendText(
        item.projectId,
        item.requesterUserId,
        formatApprovalRequest(item),
        {
          kind: 'approval-request',
          token: item.token,
          actions: [
            'approve-once',
            ...(item.persistentApprovalCommand ? ['approve-prefix' as const] : []),
            'reject'
          ]
        }
      )
    } catch (error) {
      sent = { ok: false, error: error instanceof Error ? error.message : String(error) }
    }
    if (!sent.ok) {
      this.consume(item, 'failed')
      await this.cancelFailClosed(item)
      return { ok: false, token, error: sent.error ?? 'failed to send approval request' }
    }
    // The Codex request can be resolved locally, the session can exit, or the
    // timeout can fire while Tencent delivery is awaiting its acknowledgement.
    // Never report a stale capability as successfully forwarded.
    if (
      this.byToken.get(token) !== item ||
      item.state !== 'pending' ||
      item.securityGeneration !== this.getSecurityGeneration() ||
      !this.isSecurityContextCurrent()
    ) {
      if (this.byToken.get(token) === item && item.state === 'pending') {
        this.consume(item, 'failed')
        await this.cancelFailClosed(item)
      }
      return { ok: false, token, error: 'approval is no longer pending after delivery' }
    }
    return { ok: true, token }
  }

  async handleDecision(
    input: RemoteImApprovalDecisionInput
  ): Promise<RemoteImApprovalDecisionResult> {
    const item = this.byToken.get(input.token.trim())
    const projectId = cleanRequired(input.projectId)
    const fromUserId = cleanRequired(input.fromUserId)
    // The same generic response is used for wrong users, wrong projects,
    // expired tokens and replays so the command does not become an oracle.
    if (
      !item ||
      item.projectId !== projectId ||
      item.requesterUserId !== fromUserId ||
      item.state !== 'pending' ||
      item.securityGeneration !== this.getSecurityGeneration() ||
      !this.isSecurityContextCurrent()
    ) {
      return { handled: true, ok: false, text: INVALID_APPROVAL_TEXT }
    }
    if (input.action === 'approve-prefix' && !item.persistentApprovalCommand) {
      return { handled: true, ok: false, text: INVALID_APPROVAL_TEXT }
    }
    if (this.now() >= item.expiresAt) {
      await this.expire(item)
      return { handled: true, ok: false, text: INVALID_APPROVAL_TEXT }
    }

    item.state = 'resolving'
    this.clearItemTimer(item)
    const decision: RemoteImApprovalDecision =
      input.action === 'approve-once'
        ? 'accept'
        : input.action === 'approve-prefix'
          ? 'accept-persistent'
          : 'cancel'
    let resolved: Awaited<ReturnType<RemoteImApprovalCoordinatorDeps['resolveApproval']>>
    try {
      resolved = await this.deps.resolveApproval({ ...this.requestFrom(item), decision })
    } catch (error) {
      resolved = { ok: false, error: error instanceof Error ? error.message : String(error) }
    }
    if (
      this.byToken.get(item.token) !== item ||
      (item.state !== 'resolving' && item.state !== 'resolved-awaiting-control-result')
    ) {
      return {
        handled: true,
        ok: false,
        text: '审批在处理期间已因会话、本机接管或账号切换而失效；结果未确认，请核查 Codex 与目标状态。'
      }
    }
    if (
      item.securityGeneration !== this.getSecurityGeneration() ||
      !this.isSecurityContextCurrent()
    ) {
      this.consume(item, 'session-cancelled')
      await this.cancelFailClosed(item)
      return {
        handled: true,
        ok: false,
        text: '审批处理期间授权集已变更；已尝试取消，请核查 Codex 与目标状态。'
      }
    }
    if (!resolved.ok) {
      this.consume(item, 'failed')
      if (decision !== 'cancel') await this.cancelFailClosed(item)
      return {
        handled: true,
        ok: false,
        text: `审批结果未确认，已尝试取消；请核查 Codex 与目标状态：${resolved.error ?? 'unknown error'}`
      }
    }

    if (resolved.approvalResolution?.applied === false) {
      this.sendResolutionNotice(
        item,
        resolutionOutcome({
          source: resolved.approvalResolution.winnerSource,
          decision: resolved.approvalResolution.winnerDecision
        })
      )
      this.consume(item, 'resolved')
      return {
        handled: true,
        ok: false,
        text: [
          '该审批已由另一条操作先处理，本次点击未生效。',
          `最终决定：${resolved.approvalResolution.winnerDecision}`,
          `解决来源：${resolved.approvalResolution.winnerSource}`
        ].join('\n')
      }
    }

    this.consume(item, decision === 'cancel' ? 'rejected' : 'approved')
    return {
      handled: true,
      ok: true,
      text:
        decision === 'accept'
          ? '已批准这一次命令执行。'
          : decision === 'accept-persistent'
            ? '已批准命令执行，并应用了 Codex 提供的命令前缀规则。'
            : '已拒绝这一次命令执行。'
    }
  }

  async cancelSession(sessionId: string): Promise<void> {
    const cleanSessionId = cleanRequired(sessionId)
    const pending = [...this.byToken.values()].filter(
      (item) =>
        item.sessionId === cleanSessionId &&
        (item.state === 'pending' || item.state === 'resolving')
    )
    await Promise.all(
      pending.map(async (item) => {
        this.consume(item, 'session-cancelled')
        await this.cancelFailClosed(item)
      })
    )
    for (const item of [...this.byToken.values()]) {
      if (item.sessionId === cleanSessionId) this.remove(item)
    }
  }

  async cancelAll(): Promise<void> {
    const sessionIds = new Set([...this.byToken.values()].map((item) => item.sessionId))
    await Promise.all([...sessionIds].map((sessionId) => this.cancelSession(sessionId)))
  }

  async cancelForRequesters(rawUserIds: Iterable<string>): Promise<void> {
    const userIds = new Set(
      [...rawUserIds].map((userId) => cleanRequired(userId)).filter(Boolean)
    )
    if (userIds.size === 0) return
    const pending = [...this.byToken.values()].filter(
      (item) =>
        userIds.has(item.requesterUserId) &&
        (item.state === 'pending' || item.state === 'resolving')
    )
    await Promise.all(
      pending.map(async (item) => {
        this.consume(item, 'session-cancelled')
        await this.cancelFailClosed(item)
      })
    )
    for (const item of [...this.byToken.values()]) {
      if (userIds.has(item.requesterUserId)) this.remove(item)
    }
  }

  forgetResolved(
    input: Pick<
      RemoteImApprovalRequest,
      'sessionId' | 'taskId' | 'replyId' | 'threadId' | 'turnId' | 'approvalId'
    >,
    resolution?: { source?: string; decision?: string }
  ): void {
    const token = this.tokenByIdentity.get(approvalIdentity(input))
    const item = token ? this.byToken.get(token) : undefined
    if (!item) return
    this.sendResolutionNotice(item, resolutionOutcome(resolution))
    if (item.state === 'resolving') {
      // A remote approval is resolved inside Codex before its control_result is
      // sent back to the host. The independent approval_resolved event can win
      // that transport race. Keep the capability correlated until the matching
      // RPC result arrives; only that result proves which decision was applied.
      this.clearItemTimer(item)
      item.state = 'resolved-awaiting-control-result'
      return
    }
    this.consume(item, 'resolved')
  }

  private sendResolutionNotice(
    item: PendingRemoteImApproval,
    outcome: RemoteImApprovalResolutionOutcome
  ): void {
    if (item.resolutionNoticeSent) return
    item.resolutionNoticeSent = true
    const outcomeText =
      outcome === 'auto-declined'
        ? '收到新的 IM 消息后已自动拒绝'
        : outcome === 'approved'
          ? '已批准'
          : outcome === 'rejected'
            ? '已拒绝'
            : '已处理'
    void this.deps
      .sendText(
        item.projectId,
        item.requesterUserId,
        `该审批${outcomeText}：${item.token}`,
        { kind: 'approval-resolved', token: item.token, outcome }
      )
      .catch(() => {
        // The approval itself is already resolved; this is a best-effort client-state update.
      })
  }

  private createUniqueToken(): string | null {
    for (let attempt = 0; attempt < 128; attempt += 1) {
      const token = cleanRequired(this.createToken())
      if (token && !this.byToken.has(token)) return token
    }
    return null
  }

  private requestFrom(item: PendingRemoteImApproval): RemoteImApprovalRequest {
    return {
      projectId: item.projectId,
      requesterUserId: item.requesterUserId,
      sessionId: item.sessionId,
      taskId: item.taskId,
      replyId: item.replyId,
      threadId: item.threadId,
      turnId: item.turnId,
      approvalId: item.approvalId,
      commandText: item.commandText,
      cwd: item.cwd,
      ...(item.reason ? { reason: item.reason } : {}),
      ...(item.persistentApprovalCommand
        ? { persistentApprovalCommand: item.persistentApprovalCommand }
        : {})
    }
  }

  private clearItemTimer(item: PendingRemoteImApproval): void {
    if (item.timer) this.clearTimer(item.timer)
    item.timer = null
  }

  private consume(item: PendingRemoteImApproval, state: RemoteImApprovalState): void {
    this.clearItemTimer(item)
    item.state = state
    this.pruneTerminalEntries()
  }

  private pruneTerminalEntries(): void {
    const terminal = [...this.byToken.values()]
      .filter(
        (candidate) =>
          candidate.state !== 'pending' &&
          candidate.state !== 'resolving' &&
          candidate.state !== 'resolved-awaiting-control-result'
      )
      .sort((left, right) => left.createdAt - right.createdAt)
    const excess = terminal.length - MAX_TERMINAL_APPROVAL_ENTRIES
    if (excess <= 0) return
    for (const candidate of terminal.slice(0, excess)) this.remove(candidate)
  }

  private remove(item: PendingRemoteImApproval): void {
    this.clearItemTimer(item)
    this.byToken.delete(item.token)
    this.tokenByIdentity.delete(approvalIdentity(item))
  }

  private async expire(item: PendingRemoteImApproval): Promise<void> {
    if (item.state !== 'pending') return
    this.consume(item, 'expired')
    const cancelled = await this.cancelFailClosed(item)
    // A local TUI decision or a new-IM auto-decline can win Codex's approval CAS while this
    // independent Electron timer is awaiting its cancel RPC. `approval_resolved` then changes
    // the item away from expired. Do not send a contradictory "timeout rejected" notice for a
    // timeout decision that lost that race.
    if ((item as PendingRemoteImApproval).state !== 'expired') return
    try {
      await this.deps.sendText(
        item.projectId,
        item.requesterUserId,
        cancelled
          ? `审批已超时并自动拒绝：${item.token}`
          : `审批已超时；已尝试取消但结果未确认，请核查 Codex 与目标状态：${item.token}`
      )
    } catch {
      // The timeout remains fail-closed even when its informational IM cannot be delivered.
    }
  }

  private async cancelFailClosed(item: PendingRemoteImApproval): Promise<boolean> {
    return this.cancelRequestFailClosed(this.requestFrom(item))
  }

  private async cancelRequestFailClosed(item: RemoteImApprovalRequest): Promise<boolean> {
    try {
      const result = await this.deps.resolveApproval({
        ...item,
        decision: 'cancel'
      })
      return result.ok && result.approvalResolution?.applied !== false
    } catch {
      // The session may already be gone. The local capability remains consumed.
      return false
    }
  }
}

function resolutionOutcome(input?: {
  source?: string
  decision?: string
}): RemoteImApprovalResolutionOutcome {
  if (input?.source === 'remote-im-input') return 'auto-declined'
  if (input?.decision?.startsWith('accept')) return 'approved'
  if (input?.decision === 'decline' || input?.decision === 'cancel') return 'rejected'
  return 'resolved'
}
