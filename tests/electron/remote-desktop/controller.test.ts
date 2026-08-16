import { beforeEach, describe, expect, it, vi } from 'vitest'
import {
  RemoteDesktopController,
  type RemoteDesktopSettingsSnapshot
} from '../../../electron/remote-desktop/controller.js'
import type {
  RemoteDesktopCaptureSource,
  RemoteDesktopEngine
} from '../../../electron/remote-desktop/engine.js'
import {
  decodeRemoteDesktopSignal,
  type RemoteDesktopSignal
} from '../../../electron/remote-desktop/signal.js'

const mainScreen: RemoteDesktopCaptureSource = {
  sourceId: 'screen-0',
  sourceName: 'Screen1',
  isMainScreen: true,
  width: 2560,
  height: 1600
}

function createFakeEngine(overrides: Partial<RemoteDesktopEngine> = {}) {
  const startSharing = vi.fn(async () => {})
  const stopSharing = vi.fn(async () => {})
  const listScreenSources = vi.fn(async () => [mainScreen])
  const setInputGate = vi.fn()
  return {
    engine: {
      listScreenSources,
      startSharing,
      stopSharing,
      setInputGate,
      ...overrides
    } as RemoteDesktopEngine,
    startSharing,
    stopSharing,
    listScreenSources,
    setInputGate
  }
}

function setup(settings: Partial<RemoteDesktopSettingsSnapshot> = {}, engineOverrides = {}) {
  const fake = createFakeEngine(engineOverrides)
  const sent: { toUserId: string; signal: RemoteDesktopSignal | null }[] = []
  const controller = new RemoteDesktopController({
    engine: fake.engine,
    getSettings: () => ({ mode: 'unattended', allowedUserIds: ['whq-iphone'], ...settings }),
    getCredentials: async () => ({ sdkAppId: 1600148979, userId: 'host-pc', userSig: 'sig' }),
    sendSignal: async (toUserId, text) => {
      sent.push({ toUserId, signal: decodeRemoteDesktopSignal(text) })
    }
  })
  return { controller, sent, ...fake }
}

const invite: RemoteDesktopSignal = {
  type: 'invite',
  sessionId: 's-1',
  roomId: 'mc-a-b'
}

describe('remote desktop controller (host only)', () => {
  let warn: ReturnType<typeof vi.spyOn>
  beforeEach(() => {
    warn = vi.spyOn(console, 'warn').mockImplementation(() => {})
  })

  it('accepts an invite from an allowed peer and starts sharing', async () => {
    const { controller, sent, startSharing } = setup()

    await controller.handleSignal('whq-iphone', invite)

    expect(startSharing).toHaveBeenCalledWith(
      { sdkAppId: 1600148979, userId: 'host-pc', userSig: 'sig', roomId: 'mc-a-b' },
      mainScreen
    )
    expect(controller.getState()).toEqual({
      hostState: 'sharing',
      peerUserId: 'whq-iphone',
      sessionId: 's-1'
    })
    expect(sent).toHaveLength(1)
    expect(sent[0].signal?.type).toBe('accept')
    // 几何必须随 accept 回去：主控端靠它把远端坐标换算回真实像素。
    expect(sent[0].signal?.captureGeometry).toEqual({
      sourceWidth: 2560,
      sourceHeight: 1600,
      captureX: 0,
      captureY: 0,
      captureWidth: 2560,
      captureHeight: 1600,
      contentMode: 'fit',
      revision: 1
    })
  })

  it('rejects a peer outside the allow list without starting the engine', async () => {
    const { controller, sent, startSharing, listScreenSources } = setup()

    await controller.handleSignal('stranger', invite)

    expect(startSharing).not.toHaveBeenCalled()
    // 连采集源都不该枚举：不在白名单的人不配触发本机任何硬件动作。
    expect(listScreenSources).not.toHaveBeenCalled()
    expect(sent[0].signal?.type).toBe('reject')
    expect(sent[0].signal?.reason).toBe('你不在对方的允许列表中')
    expect(controller.getState().hostState).toBe('idle')
  })

  it('rejects when the mode is disabled', async () => {
    const { controller, sent, startSharing } = setup({ mode: 'disabled' })

    await controller.handleSignal('whq-iphone', invite)

    expect(startSharing).not.toHaveBeenCalled()
    expect(sent[0].signal?.reason).toBe('对方未开启远程桌面')
  })

  it('answers instead of going silent when attended mode is not implemented yet', async () => {
    // 静默不响应会让对端一直卡在"连接中"，比明确拒绝更糟。
    const { controller, sent, startSharing } = setup({ mode: 'attended' })

    await controller.handleSignal('whq-iphone', invite)

    expect(startSharing).not.toHaveBeenCalled()
    expect(sent[0].signal?.type).toBe('reject')
    expect(sent[0].signal?.reason).toContain('尚未支持')
  })

  it('reports a failure to enter the room instead of leaving the peer hanging', async () => {
    const { controller, sent } = setup(
      {},
      { startSharing: vi.fn(async () => { throw new Error('进房失败: -3301') }) }
    )

    await controller.handleSignal('whq-iphone', invite)

    expect(sent[0].signal?.type).toBe('reject')
    expect(sent[0].signal?.reason).toBe('进房失败: -3301')
    // 失败后必须回到 idle，否则这台机器会永远拒绝后续邀请（busy）。
    expect(controller.getState().hostState).toBe('idle')
  })

  it('rejects an invite without a room id', async () => {
    const { controller, sent, startSharing } = setup()

    await controller.handleSignal('whq-iphone', { type: 'invite', sessionId: 's-1' })

    expect(startSharing).not.toHaveBeenCalled()
    expect(sent[0].signal?.reason).toBe('邀请缺少房间号')
  })

  it('does not let a stranger stop an active session', async () => {
    const { controller, stopSharing } = setup()
    await controller.handleSignal('whq-iphone', invite)

    await controller.handleSignal('stranger', { type: 'stop', sessionId: 's-1' })

    expect(stopSharing).not.toHaveBeenCalled()
    expect(controller.getState().hostState).toBe('sharing')

    await controller.handleSignal('whq-iphone', { type: 'stop', sessionId: 's-1' })
    expect(stopSharing).toHaveBeenCalledTimes(1)
    expect(controller.getState().hostState).toBe('idle')
  })

  it('tells the peer when sharing is stopped locally', async () => {
    const { controller, sent, stopSharing } = setup()
    await controller.handleSignal('whq-iphone', invite)

    await controller.stopByLocalUser()

    expect(stopSharing).toHaveBeenCalledTimes(1)
    expect(sent[1].signal?.type).toBe('stop')
    expect(sent[1].toUserId).toBe('whq-iphone')
    expect(controller.getState().hostState).toBe('idle')
  })

  it('refuses a second invite while already sharing', async () => {
    const { controller, sent, startSharing } = setup()
    await controller.handleSignal('whq-iphone', invite)

    await controller.handleSignal('whq-iphone', { type: 'invite', sessionId: 's-2', roomId: 'r2' })

    expect(startSharing).toHaveBeenCalledTimes(1)
    expect(sent[1].signal?.reason).toBe('对方正在共享中')
    // 被占用时不能把已有会话状态打回 idle。
    expect(controller.getState().sessionId).toBe('s-1')
  })

  it('ignores viewer-side signals because this app is host only', async () => {
    // Multi-AI Code 不能作主控端：收到 accept/reject/notice 说明角色搞反或是旧残留，
    // 记录但绝不触发任何动作。
    const { controller, startSharing, stopSharing } = setup()

    for (const type of ['accept', 'reject', 'notice'] as const) {
      const consumed = await controller.handleSignal('whq-iphone', {
        type,
        sessionId: 's-1',
        ...(type === 'notice' ? { noticeCode: 'secure-desktop-entered' } : {})
      })
      expect(consumed, type).toBe(true)
    }
    expect(startSharing).not.toHaveBeenCalled()
    expect(stopSharing).not.toHaveBeenCalled()
    expect(controller.getState().hostState).toBe('idle')
    warn.mockRestore()
  })

  describe('remote control gate', () => {
    it('keeps input off when the user only allowed viewing', async () => {
      // 开"看屏幕"不等于把整台电脑交出去：控制是独立的一档授权。
      const { controller, setInputGate } = setup({ allowRemoteControl: false })
      await controller.handleSignal('whq-iphone', invite)

      expect(setInputGate).toHaveBeenCalledWith(null)
    })

    it('opens the gate bound to this session, peer and screen', async () => {
      const { controller, setInputGate } = setup({ allowRemoteControl: true })
      await controller.handleSignal('whq-iphone', invite)

      expect(setInputGate).toHaveBeenCalledWith({
        sessionId: invite.sessionId,
        peerUserId: 'whq-iphone',
        // 采集的是整屏，归一化坐标按这块屏换算回像素。
        captureScreen: { left: 0, top: 0, width: mainScreen.width, height: mainScreen.height }
      })
    })

    it('closes the gate when the local user stops sharing', async () => {
      // 关掉共享的那一刻必须收回控制，否则对方最后按住的键会永远卡在这台电脑上。
      const { controller, setInputGate } = setup({ allowRemoteControl: true })
      await controller.handleSignal('whq-iphone', invite)
      setInputGate.mockClear()

      await controller.stopByLocalUser()

      expect(setInputGate).toHaveBeenCalledWith(null)
    })

    it('closes the gate when the peer goes away', async () => {
      const { controller, setInputGate } = setup({ allowRemoteControl: true })
      await controller.handleSignal('whq-iphone', invite)
      setInputGate.mockClear()

      await controller.handleSignal('whq-iphone', { type: 'stop', sessionId: invite.sessionId })

      expect(setInputGate).toHaveBeenCalledWith(null)
    })

    it('closes the gate when entering the room failed', async () => {
      // 共享没起来却把输入放开，等于对着一台看不见的电脑瞎点。
      const { controller, setInputGate } = setup({ allowRemoteControl: true }, {
        startSharing: vi.fn(async () => {
          throw new Error('boom')
        })
      })
      await controller.handleSignal('whq-iphone', invite)

      expect(setInputGate).toHaveBeenCalledWith(null)
      expect(setInputGate).not.toHaveBeenCalledWith(expect.objectContaining({ peerUserId: 'whq-iphone' }))
    })
  })
})
