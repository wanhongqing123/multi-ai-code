import { describe, expect, it, vi } from 'vitest'
import {
  RemoteImApprovalCoordinator,
  type RemoteImApprovalDecision,
  type RemoteImApprovalRequest,
  type RemoteImApprovalResolution
} from '../../../electron/remote-im/approvalCoordinator.js'

const request: RemoteImApprovalRequest = {
  projectId: 'project-a',
  requesterUserId: 'phone-a',
  sessionId: 'session-a',
  taskId: 'task-a',
  replyId: 'reply-a',
  threadId: 'thread-a',
  turnId: 'turn-a',
  approvalId: 'approval-internal-a',
  commandText: 'Remove-Item -LiteralPath C:\\repo\\tmp -Recurse -Force',
  cwd: 'C:\\repo',
  reason: '清理构建缓存'
}

describe('RemoteImApprovalCoordinator', () => {
  it('sends versioned button actions and resolves the persistent prefix decision', async () => {
    const sent: Array<{ text: string; interaction: unknown }> = []
    const resolutions: RemoteImApprovalResolution[] = []
    const persistentRequest: RemoteImApprovalRequest = {
      ...request,
      persistentApprovalCommand: 'Remove-Item -LiteralPath C:\\repo\\tmp -Recurse -Force'
    }
    const coordinator = new RemoteImApprovalCoordinator({
      createToken: () => 'approval-public-prefix',
      sendText: async (_projectId, _toUserId, text, interaction) => {
        sent.push({ text, interaction })
        return { ok: true }
      },
      resolveApproval: async (input) => {
        resolutions.push(input)
        return { ok: true }
      }
    })

    await coordinator.register(persistentRequest)

    expect(sent[0]?.text).toContain('请使用 MaiChat 消息卡片下方的按钮')
    expect(sent[0]?.text).toContain('“同意并记住”将记住以下命令前缀')
    expect(sent[0]?.text).not.toContain('/approve')
    expect(sent[0]?.interaction).toEqual({
      kind: 'approval-request',
      token: 'approval-public-prefix',
      actions: ['approve-once', 'approve-prefix', 'reject']
    })

    await expect(
      coordinator.handleDecision({
        projectId: 'project-a',
        fromUserId: 'phone-a',
        token: 'approval-public-prefix',
        action: 'approve-prefix'
      })
    ).resolves.toMatchObject({ handled: true, ok: true })
    expect(resolutions).toEqual([{ ...persistentRequest, decision: 'accept-persistent' }])
  })

  it('rejects a persistent approval command when Codex did not offer that choice', async () => {
    const resolutions: RemoteImApprovalResolution[] = []
    const coordinator = new RemoteImApprovalCoordinator({
      createToken: () => 'approval-public-no-prefix',
      sendText: async () => ({ ok: true }),
      resolveApproval: async (input) => {
        resolutions.push(input)
        return { ok: true }
      }
    })
    await coordinator.register(request)

    await expect(
      coordinator.handleDecision({
        projectId: 'project-a',
        fromUserId: 'phone-a',
        token: 'approval-public-no-prefix',
        action: 'approve-prefix'
      })
    ).resolves.toMatchObject({ handled: true, ok: false })
    expect(resolutions).toEqual([])
  })

  it('binds a one-time approval to project, requester, session, task and thread', async () => {
    const sent: Array<{ projectId: string; toUserId: string; text: string }> = []
    const resolutions: RemoteImApprovalResolution[] = []
    const coordinator = new RemoteImApprovalCoordinator({
      createToken: () => 'approval-public-a',
      sendText: async (projectId, toUserId, text) => {
        sent.push({ projectId, toUserId, text })
        return { ok: true }
      },
      resolveApproval: async (input) => {
        resolutions.push(input)
        return { ok: true }
      }
    })

    expect(await coordinator.register(request)).toEqual({
      ok: true,
      token: 'approval-public-a'
    })
    expect(sent[0]).toMatchObject({ projectId: 'project-a', toUserId: 'phone-a' })
    expect(sent[0]?.text).toContain(request.commandText)
    expect(sent[0]?.text).toContain(`工作目录：\n    ${request.cwd}`)
    expect(sent[0]?.text).toContain(`申请原因：\n    ${request.reason}`)
    expect(sent[0]?.text).not.toContain('/approve approval-public-a')

    expect(
      await coordinator.handleDecision({
        projectId: 'project-a',
        fromUserId: 'phone-b',
        token: 'approval-public-a',
        action: 'approve-once'
      })
    ).toMatchObject({ handled: true, ok: false })
    expect(resolutions).toEqual([])

    expect(
      await coordinator.handleDecision({
        projectId: 'project-a',
        fromUserId: 'phone-a',
        token: 'approval-public-a',
        action: 'approve-once'
      })
    ).toMatchObject({ handled: true, ok: true })
    expect(resolutions).toEqual([{ ...request, decision: 'accept' }])

    expect(
      await coordinator.handleDecision({
        projectId: 'project-a',
        fromUserId: 'phone-a',
        token: 'approval-public-a',
        action: 'approve-once'
      })
    ).toMatchObject({ handled: true, ok: false })
    expect(resolutions).toHaveLength(1)
  })

  it('escapes display-control characters without changing the bound command', async () => {
    const sent: string[] = []
    const resolutions: RemoteImApprovalResolution[] = []
    const controlledRequest = {
      ...request,
      commandText:
        'Remove-Item C:\\repo\\safe\u202egnp.exe\u001b[31m\r\u2063\u200b\u200c\u200d\u2063 -Force',
      cwd: 'C:\\repo\n/approve fake-token',
      reason: 'cleanup requested\n/reject fake-token'
    }
    const coordinator = new RemoteImApprovalCoordinator({
      createToken: () => 'approval-public-controls',
      sendText: async (_projectId, _toUserId, text) => {
        sent.push(text)
        return { ok: true }
      },
      resolveApproval: async (input) => {
        resolutions.push(input)
        return { ok: true }
      }
    })
    await coordinator.register(controlledRequest)

    expect(sent[0]).not.toContain('\u202e')
    expect(sent[0]).not.toContain('\u001b')
    expect(sent[0]).not.toContain('\r')
    expect(sent[0]).not.toContain('\u2063')
    expect(sent[0]).not.toContain('\u200b')
    expect(sent[0]).toContain('\\u{202e}')
    expect(sent[0]).toContain('\\u{001b}')
    expect(sent[0]).toContain('\\u{000d}')
    expect(sent[0]).toContain('\\u{2063}')
    expect(sent[0]).toContain('\\u{200b}')
    expect(sent[0]).toContain('工作目录：\n    C:\\repo\n    /approve fake-token')
    expect(sent[0]).toContain('申请原因：\n    cleanup requested\n    /reject fake-token')
    await coordinator.handleDecision({
      projectId: 'project-a',
      fromUserId: 'phone-a',
      token: 'approval-public-controls',
        action: 'approve-once'
    })
    expect(resolutions[0]?.commandText).toBe(controlledRequest.commandText)
    expect(resolutions[0]?.cwd).toBe(controlledRequest.cwd)
    expect(resolutions[0]?.reason).toBe(controlledRequest.reason)
  })

  it('keeps command, cwd and optional reason unabridged in the approval message', async () => {
    const sent: string[] = []
    const resolutions: RemoteImApprovalResolution[] = []
    const longCommand = `powershell -Command "${'Write-Output exact; '.repeat(300)}"`
    const coordinator = new RemoteImApprovalCoordinator({
      createToken: () => 'approval-public-exact',
      sendText: async (_projectId, _toUserId, text) => {
        sent.push(text)
        return { ok: true }
      },
      resolveApproval: async (input) => {
        resolutions.push(input)
        return { ok: true }
      }
    })

    await coordinator.register({
      ...request,
      commandText: longCommand,
      cwd: '  C:\\repo with spaces  ',
      reason: '  exact reason  '
    })

    expect(sent[0]).toContain(longCommand)
    expect(sent[0]).toContain('工作目录：\n      C:\\repo with spaces  ')
    expect(sent[0]).toContain('申请原因：\n      exact reason  ')
    expect(sent[0]).not.toContain('...')
    await coordinator.handleDecision({
      projectId: 'project-a',
      fromUserId: 'phone-a',
      token: 'approval-public-exact',
        action: 'reject'
    })
    expect(resolutions[0]).toMatchObject({
      cwd: '  C:\\repo with spaces  ',
      reason: '  exact reason  '
    })
  })

  it('deduplicates reliable approval events without issuing another capability', async () => {
    const sendText = vi.fn(async () => ({ ok: true }))
    const coordinator = new RemoteImApprovalCoordinator({
      createToken: () => 'approval-public-a',
      sendText,
      resolveApproval: async () => ({ ok: true })
    })

    await expect(coordinator.register(request)).resolves.toEqual({
      ok: true,
      token: 'approval-public-a'
    })
    await expect(coordinator.register({ ...request })).resolves.toEqual({
      ok: true,
      token: 'approval-public-a'
    })
    expect(sendText).toHaveBeenCalledTimes(1)

    coordinator.forgetResolved(request)
    await expect(coordinator.register({ ...request })).resolves.toMatchObject({
      ok: false,
      error: 'approval was already resolved'
    })
    expect(sendText).toHaveBeenCalledTimes(1)

    await expect(
      coordinator.register({ ...request, commandText: 'Remove-Item C:\\other -Force' })
    ).resolves.toMatchObject({ ok: false, error: 'approval identity collision' })
  })

  it('does not report a capability as forwarded when it resolves during delivery', async () => {
    let finishDelivery: ((result: { ok: boolean }) => void) | undefined
    const coordinator = new RemoteImApprovalCoordinator({
      createToken: () => 'approval-public-delivery-race',
      sendText: () =>
        new Promise((resolve) => {
          finishDelivery = resolve
        }),
      resolveApproval: async () => ({ ok: true })
    })

    const registration = coordinator.register(request)
    await vi.waitFor(() => expect(finishDelivery).toBeTypeOf('function'))
    coordinator.forgetResolved(request)
    finishDelivery?.({ ok: true })

    await expect(registration).resolves.toMatchObject({
      ok: false,
      error: 'approval is no longer pending after delivery'
    })
  })

  it('cancels a delivered token when authority changes while its SDK ack is pending', async () => {
    let authorityCurrent = true
    let finishDelivery: ((result: { ok: boolean }) => void) | undefined
    const resolutions: RemoteImApprovalResolution[] = []
    const coordinator = new RemoteImApprovalCoordinator({
      createToken: () => 'approval-delivery-authority-race',
      isSecurityContextCurrent: () => authorityCurrent,
      sendText: () =>
        new Promise((resolve) => {
          finishDelivery = resolve
        }),
      resolveApproval: async (input) => {
        resolutions.push(input)
        return { ok: true }
      }
    })

    const registration = coordinator.register(request)
    await vi.waitFor(() => expect(finishDelivery).toBeTypeOf('function'))
    authorityCurrent = false
    finishDelivery?.({ ok: true })
    await expect(registration).resolves.toMatchObject({ ok: false })
    expect(resolutions).toEqual([{ ...request, decision: 'cancel' }])

    authorityCurrent = true
    await expect(
      coordinator.handleDecision({
        projectId: 'project-a',
        fromUserId: 'phone-a',
        token: 'approval-delivery-authority-race',
        action: 'approve-once'
      })
    ).resolves.toMatchObject({ handled: true, ok: false })
  })

  it('automatically cancels on timeout and rejects expired or replayed tokens', async () => {
    vi.useFakeTimers()
    try {
      let now = 1_000
      const resolutions: RemoteImApprovalResolution[] = []
      const notices: string[] = []
      const coordinator = new RemoteImApprovalCoordinator({
        timeoutMs: 10 * 60 * 1000,
        now: () => now,
        createToken: () => 'approval-public-timeout',
        sendText: async (_projectId, _toUserId, text) => {
          notices.push(text)
          return { ok: true }
        },
        resolveApproval: async (input) => {
          resolutions.push(input)
          return { ok: true }
        }
      })
      await coordinator.register(request)
      now += 10 * 60 * 1000
      await vi.advanceTimersByTimeAsync(10 * 60 * 1000)

      expect(resolutions).toEqual([{ ...request, decision: 'cancel' }])
      expect(notices.at(-1)).toContain('自动拒绝')
      await expect(
        coordinator.handleDecision({
          projectId: 'project-a',
          fromUserId: 'phone-a',
          token: 'approval-public-timeout',
        action: 'approve-once'
        })
      ).resolves.toMatchObject({ handled: true, ok: false })
      expect(resolutions).toHaveLength(1)
    } finally {
      vi.useRealTimers()
    }
  })

  it('cancels all still-pending approvals when the AICLI session exits', async () => {
    const resolutions: RemoteImApprovalResolution[] = []
    let token = 0
    const coordinator = new RemoteImApprovalCoordinator({
      createToken: () => `approval-public-${++token}`,
      sendText: async () => ({ ok: true }),
      resolveApproval: async (input) => {
        resolutions.push(input)
        return { ok: true }
      }
    })
    await coordinator.register(request)
    await coordinator.register({
      ...request,
      taskId: 'task-b',
      approvalId: 'approval-internal-b'
    })
    await coordinator.register({
      ...request,
      sessionId: 'session-b',
      taskId: 'task-c',
      approvalId: 'approval-internal-c'
    })

    await coordinator.cancelSession('session-a')
    expect(resolutions).toEqual([
      { ...request, decision: 'cancel' },
      { ...request, taskId: 'task-b', approvalId: 'approval-internal-b', decision: 'cancel' }
    ])
  })

  it('invalidates old-account tokens and cancels them during account teardown', async () => {
    let securityGeneration = 7
    const resolutions: RemoteImApprovalResolution[] = []
    const coordinator = new RemoteImApprovalCoordinator({
      getSecurityGeneration: () => securityGeneration,
      createToken: () => 'approval-public-old-account',
      sendText: async () => ({ ok: true }),
      resolveApproval: async (input) => {
        resolutions.push(input)
        return { ok: true }
      }
    })
    await coordinator.register(request)

    securityGeneration += 1
    await expect(
      coordinator.handleDecision({
        projectId: 'project-a',
        fromUserId: 'phone-a',
        token: 'approval-public-old-account',
        action: 'approve-once'
      })
    ).resolves.toMatchObject({ handled: true, ok: false })
    expect(resolutions).toEqual([])

    await coordinator.cancelAll()
    expect(resolutions).toEqual([{ ...request, decision: 'cancel' }])
  })

  it('revokes only approvals owned by contacts whose authority was removed', async () => {
    const resolutions: RemoteImApprovalResolution[] = []
    let token = 0
    const coordinator = new RemoteImApprovalCoordinator({
      createToken: () => `approval-contact-${++token}`,
      sendText: async () => ({ ok: true }),
      resolveApproval: async (input) => {
        resolutions.push(input)
        return { ok: true }
      }
    })
    await coordinator.register(request)
    await coordinator.register({
      ...request,
      requesterUserId: 'phone-b',
      taskId: 'task-b',
      replyId: 'reply-b',
      turnId: 'turn-b',
      approvalId: 'approval-b'
    })

    await coordinator.cancelForRequesters(['phone-a'])
    await expect(
      coordinator.handleDecision({
        projectId: 'project-a',
        fromUserId: 'phone-a',
        token: 'approval-contact-1',
        action: 'approve-once'
      })
    ).resolves.toMatchObject({ handled: true, ok: false })
    await expect(
      coordinator.handleDecision({
        projectId: 'project-a',
        fromUserId: 'phone-b',
        token: 'approval-contact-2',
        action: 'approve-once'
      })
    ).resolves.toMatchObject({ handled: true, ok: true })
    expect(resolutions.map((item) => [item.requesterUserId, item.decision])).toEqual([
      ['phone-a', 'cancel'],
      ['phone-b', 'accept']
    ])
  })

  it('rejects a command while the contact authority set is mutating', async () => {
    let authorityCurrent = true
    const resolutions: RemoteImApprovalResolution[] = []
    const coordinator = new RemoteImApprovalCoordinator({
      isSecurityContextCurrent: () => authorityCurrent,
      createToken: () => 'approval-authority-changing',
      sendText: async () => ({ ok: true }),
      resolveApproval: async (input) => {
        resolutions.push(input)
        return { ok: true }
      }
    })
    await coordinator.register(request)
    authorityCurrent = false

    await expect(
      coordinator.handleDecision({
        projectId: 'project-a',
        fromUserId: 'phone-a',
        token: 'approval-authority-changing',
        action: 'approve-once'
      })
    ).resolves.toMatchObject({ handled: true, ok: false })
    expect(resolutions).toEqual([])
  })

  it('does not report an in-flight approval as successful after session cancellation', async () => {
    let finishAccept: ((result: { ok: boolean }) => void) | undefined
    const decisions: RemoteImApprovalDecision[] = []
    const coordinator = new RemoteImApprovalCoordinator({
      createToken: () => 'approval-public-in-flight',
      sendText: async () => ({ ok: true }),
      resolveApproval: (input) => {
        decisions.push(input.decision)
        if (input.decision === 'cancel') return Promise.resolve({ ok: true })
        return new Promise((resolve) => {
          finishAccept = resolve
        })
      }
    })
    await coordinator.register(request)

    const approving = coordinator.handleDecision({
      projectId: 'project-a',
      fromUserId: 'phone-a',
      token: 'approval-public-in-flight',
        action: 'approve-once'
    })
    await vi.waitFor(() => expect(finishAccept).toBeTypeOf('function'))
    await coordinator.cancelAll()
    finishAccept?.({ ok: true })

    await expect(approving).resolves.toMatchObject({ handled: true, ok: false })
    expect(decisions).toEqual(['accept', 'cancel'])
  })

  it('reports success when approval_resolved arrives before the matching control result', async () => {
    let finishAccept: ((result: { ok: boolean }) => void) | undefined
    const decisions: RemoteImApprovalDecision[] = []
    const coordinator = new RemoteImApprovalCoordinator({
      createToken: () => 'approval-public-resolved-before-control-result',
      sendText: async () => ({ ok: true }),
      resolveApproval: (input) => {
        decisions.push(input.decision)
        return new Promise((resolve) => {
          finishAccept = resolve
        })
      }
    })
    await coordinator.register(request)

    const approving = coordinator.handleDecision({
      projectId: 'project-a',
      fromUserId: 'phone-a',
      token: 'approval-public-resolved-before-control-result',
        action: 'approve-once'
    })
    await vi.waitFor(() => expect(finishAccept).toBeTypeOf('function'))

    coordinator.forgetResolved(request)
    finishAccept?.({ ok: true })

    await expect(approving).resolves.toEqual({
      handled: true,
      ok: true,
      text: '已批准这一次命令执行。'
    })
    expect(decisions).toEqual(['accept'])
  })

  it('cancels an in-flight approval when authority changes before the RPC returns', async () => {
    let authorityCurrent = true
    let finishAccept: ((result: { ok: boolean }) => void) | undefined
    const decisions: RemoteImApprovalDecision[] = []
    const coordinator = new RemoteImApprovalCoordinator({
      createToken: () => 'approval-authority-race',
      isSecurityContextCurrent: () => authorityCurrent,
      sendText: async () => ({ ok: true }),
      resolveApproval: (input) => {
        decisions.push(input.decision)
        if (input.decision === 'cancel') return Promise.resolve({ ok: true })
        return new Promise((resolve) => {
          finishAccept = resolve
        })
      }
    })
    await coordinator.register(request)

    const approving = coordinator.handleDecision({
      projectId: 'project-a',
      fromUserId: 'phone-a',
      token: 'approval-authority-race',
        action: 'approve-once'
    })
    await vi.waitFor(() => expect(finishAccept).toBeTypeOf('function'))
    authorityCurrent = false
    finishAccept?.({ ok: true })

    await expect(approving).resolves.toMatchObject({ handled: true, ok: false })
    expect(decisions).toEqual(['accept', 'cancel'])
  })

  it('consumes an approval when resolution fails and attempts a fail-closed cancel', async () => {
    const decisions: string[] = []
    const coordinator = new RemoteImApprovalCoordinator({
      createToken: () => 'approval-public-failed',
      sendText: async () => ({ ok: true }),
      resolveApproval: async ({ decision }) => {
        decisions.push(decision)
        return decision === 'cancel' ? { ok: true } : { ok: false, error: 'bridge failed' }
      }
    })
    await coordinator.register(request)

    const first = await coordinator.handleDecision({
      projectId: 'project-a',
      fromUserId: 'phone-a',
      token: 'approval-public-failed',
        action: 'approve-once'
    })
    expect(first).toMatchObject({ handled: true, ok: false })
    expect(decisions).toEqual(['accept', 'cancel'])

    const replay = await coordinator.handleDecision({
      projectId: 'project-a',
      fromUserId: 'phone-a',
      token: 'approval-public-failed',
        action: 'approve-once'
    })
    expect(replay).toMatchObject({ handled: true, ok: false })
    expect(decisions).toHaveLength(2)
  })

  it('invalidates the IM capability when Codex resolves the approval elsewhere', async () => {
    const coordinator = new RemoteImApprovalCoordinator({
      createToken: () => 'approval-public-local-overlay',
      sendText: async () => ({ ok: true }),
      resolveApproval: async () => ({ ok: true })
    })
    await coordinator.register(request)
    coordinator.forgetResolved(request)

    await expect(
      coordinator.handleDecision({
        projectId: 'project-a',
        fromUserId: 'phone-a',
        token: 'approval-public-local-overlay',
        action: 'approve-once'
      })
    ).resolves.toMatchObject({ handled: true, ok: false })
  })

  it('does not let an old resolved event invalidate a newer task capability', async () => {
    let token = 0
    const coordinator = new RemoteImApprovalCoordinator({
      createToken: () => `approval-public-next-${++token}`,
      sendText: async () => ({ ok: true }),
      resolveApproval: async () => ({ ok: true })
    })
    const nextRequest = {
      ...request,
      taskId: 'task-next',
      turnId: 'turn-next'
    }
    await coordinator.register(request)
    await coordinator.register(nextRequest)

    coordinator.forgetResolved(request)

    await expect(
      coordinator.handleDecision({
        projectId: 'project-a',
        fromUserId: 'phone-a',
        token: 'approval-public-next-2',
        action: 'approve-once'
      })
    ).resolves.toMatchObject({ handled: true, ok: true })
  })

  it('fails closed when the bounded per-session approval registry is full', async () => {
    const resolutions: RemoteImApprovalResolution[] = []
    let token = 0
    const coordinator = new RemoteImApprovalCoordinator({
      maxEntriesPerSession: 1,
      createToken: () => `approval-public-cap-${++token}`,
      sendText: async () => ({ ok: true }),
      resolveApproval: async (input) => {
        resolutions.push(input)
        return { ok: true }
      }
    })
    await coordinator.register(request)
    await expect(
      coordinator.register({
        ...request,
        taskId: 'task-cap-2',
        approvalId: 'approval-internal-cap-2'
      })
    ).resolves.toMatchObject({ ok: false, error: 'too many approval requests in this session' })
    expect(resolutions).toEqual([
      {
        ...request,
        taskId: 'task-cap-2',
        approvalId: 'approval-internal-cap-2',
        decision: 'cancel'
      }
    ])
  })

  it('fails closed instead of looping when token allocation cannot find a unique value', async () => {
    const resolutions: RemoteImApprovalResolution[] = []
    const coordinator = new RemoteImApprovalCoordinator({
      createToken: () => 'approval-collision',
      sendText: async () => ({ ok: true }),
      resolveApproval: async (input) => {
        resolutions.push(input)
        return { ok: true }
      }
    })
    await expect(coordinator.register(request)).resolves.toMatchObject({ ok: true })
    const collidingRequest = {
      ...request,
      sessionId: 'session-collision',
      taskId: 'task-collision',
      replyId: 'reply-collision',
      turnId: 'turn-collision',
      approvalId: 'approval-collision-internal'
    }

    await expect(coordinator.register(collidingRequest)).resolves.toEqual({
      ok: false,
      error: 'failed to allocate a unique approval token'
    })
    expect(resolutions).toEqual([{ ...collidingRequest, decision: 'cancel' }])
  })
})
