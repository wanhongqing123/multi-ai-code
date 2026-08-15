import { describe, expect, it } from 'vitest'
import {
  REMOTE_DESKTOP_MAX_CONSECUTIVE_FAILURES,
  REMOTE_DESKTOP_REJECT_REASONS,
  decideOnConsentResult,
  decideOnInvite,
  decideOnPeerGone,
  remoteInputVerdict,
  shouldAcceptRemoteInput,
  type RemoteDesktopInputGateInput,
  type RemoteDesktopInviteInput
} from '../../../electron/remote-desktop/session.js'

function invite(overrides: Partial<RemoteDesktopInviteInput> = {}): RemoteDesktopInviteInput {
  return {
    mode: 'unattended',
    currentState: 'idle',
    senderAllowed: true,
    passwordConfigured: false,
    authProofValid: false,
    consecutiveFailures: 0,
    ...overrides
  }
}

function gate(
  overrides: Partial<RemoteDesktopInputGateInput> = {}
): RemoteDesktopInputGateInput {
  return {
    currentState: 'sharing',
    allowRemoteControl: true,
    currentSessionId: 's-1',
    currentPeerUserId: 'whq-iphone',
    packetSessionId: 's-1',
    fromUserId: 'whq-iphone',
    ...overrides
  }
}

describe('remote desktop host session', () => {
  it('checks the allow list before the mode so config never leaks', () => {
    // 顺序反过来就会泄漏这台机器的配置：不在白名单的人能通过拒绝理由
    // 分辨出"对方开没开远程桌面"。
    const disabledAndNotAllowed = decideOnInvite({
      ...invite({ mode: 'disabled', senderAllowed: false })
    })
    expect(disabledAndNotAllowed.action).toBe('rejectInvite')
    expect(disabledAndNotAllowed.reason).toBe(REMOTE_DESKTOP_REJECT_REASONS.notAllowed)

    const allowedButDisabled = decideOnInvite(invite({ mode: 'disabled' }))
    expect(allowedButDisabled.reason).toBe(REMOTE_DESKTOP_REJECT_REASONS.notEnabled)
  })

  it('never preempts an existing session', () => {
    for (const currentState of ['sharing', 'awaitingConsent'] as const) {
      const decision = decideOnInvite(invite({ currentState }))
      expect(decision.action, currentState).toBe('rejectInvite')
      expect(decision.reason, currentState).toBe(REMOTE_DESKTOP_REJECT_REASONS.busy)
      // 拒绝不能把已有会话打回 idle，否则一条邀请就能踢掉正在进行的共享。
      expect(decision.nextState, currentState).toBe(currentState)
    }
  })

  it('prompts in attended mode and shares directly in unattended mode', () => {
    expect(decideOnInvite(invite({ mode: 'attended' }))).toEqual({
      action: 'showConsentDialog',
      nextState: 'awaitingConsent'
    })
    expect(decideOnInvite(invite({ mode: 'unattended' }))).toEqual({
      action: 'acceptAndShare',
      nextState: 'sharing'
    })
  })

  it('requires the password only when one is configured', () => {
    // 没设密码时白名单即授权：主场景是自己连自己的电脑，不该有额外摩擦。
    expect(decideOnInvite(invite({ passwordConfigured: false, authProofValid: false })).action).toBe(
      'acceptAndShare'
    )

    const wrong = decideOnInvite(invite({ passwordConfigured: true, authProofValid: false }))
    expect(wrong.action).toBe('rejectInvite')
    expect(wrong.reason).toBe(REMOTE_DESKTOP_REJECT_REASONS.badPassword)

    expect(decideOnInvite(invite({ passwordConfigured: true, authProofValid: true })).action).toBe(
      'acceptAndShare'
    )
  })

  it('downgrades to attended after repeated password failures', () => {
    // 防在线爆破：连续失败到阈值就退回每次确认。
    const justBelow = decideOnInvite(
      invite({
        passwordConfigured: true,
        authProofValid: false,
        consecutiveFailures: REMOTE_DESKTOP_MAX_CONSECUTIVE_FAILURES - 2
      })
    )
    expect(justBelow.downgradeToAttended).toBe(false)

    const atThreshold = decideOnInvite(
      invite({
        passwordConfigured: true,
        authProofValid: false,
        consecutiveFailures: REMOTE_DESKTOP_MAX_CONSECUTIVE_FAILURES - 1
      })
    )
    expect(atThreshold.downgradeToAttended).toBe(true)
  })

  it('only reacts to a consent result while actually awaiting one', () => {
    expect(decideOnConsentResult('awaitingConsent', true)).toEqual({
      action: 'acceptAndShare',
      nextState: 'sharing'
    })
    expect(decideOnConsentResult('awaitingConsent', false)).toEqual({
      action: 'rejectInvite',
      nextState: 'idle',
      reason: REMOTE_DESKTOP_REJECT_REASONS.declined
    })
    // 乱序到达（例如弹窗已超时自动拒绝后用户又点了一下）不能把状态搅乱。
    for (const state of ['idle', 'sharing'] as const) {
      expect(decideOnConsentResult(state, true)).toEqual({ action: 'none', nextState: state })
    }
  })

  it('stops sharing when the peer goes away and is a no-op otherwise', () => {
    expect(decideOnPeerGone('sharing')).toEqual({ action: 'stopSharing', nextState: 'idle' })
    expect(decideOnPeerGone('awaitingConsent')).toEqual({ action: 'none', nextState: 'idle' })
    expect(decideOnPeerGone('idle')).toEqual({ action: 'none', nextState: 'idle' })
  })

  it('gates remote input on four independent conditions', () => {
    expect(remoteInputVerdict(gate())).toBe('accepted')
    expect(shouldAcceptRemoteInput(gate())).toBe(true)

    // 四种拒绝对外都表现为"远端鼠标不动"，必须能分辨是哪一种。
    expect(remoteInputVerdict(gate({ currentState: 'idle' }))).toBe('not-sharing')
    expect(remoteInputVerdict(gate({ allowRemoteControl: false }))).toBe('control-disabled')
    expect(remoteInputVerdict(gate({ packetSessionId: 's-old' }))).toBe('session-mismatch')
    expect(remoteInputVerdict(gate({ currentSessionId: '' }))).toBe('session-mismatch')
    expect(remoteInputVerdict(gate({ fromUserId: 'someone-else' }))).toBe('peer-mismatch')
    expect(remoteInputVerdict(gate({ currentPeerUserId: '' }))).toBe('peer-mismatch')
  })

  it('keeps watching and controlling as separate permissions', () => {
    // 无人值守只授权了"看"。控制是独立开关，否则同一个开关会在用户
    // 毫不知情的情况下从"能看"放大成"能完全操作我的电脑"。
    const watching = decideOnInvite(invite({ mode: 'unattended' }))
    expect(watching.action).toBe('acceptAndShare')
    expect(remoteInputVerdict(gate({ allowRemoteControl: false }))).toBe('control-disabled')
  })
})
