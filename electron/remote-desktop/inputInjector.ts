// 被控端输入注入的判定层。
//
// 分两层（与 MaiChat 的 RemoteInputInjector 同构）：
//   RemoteInputSink     —— 真正落到系统的原子操作（Windows SendInput）
//   createRemoteInputInjector —— 会话绑定、按住跟踪、丢包兜底、危险组合键拦截
//
// 所有判定都在本文件且不依赖任何平台 API，可以拿假 sink 完整单测；
// 平台相关的部分被压缩到 sink 那一层，薄到不需要测。

import type { RemoteInputChannel, RemoteInputPacket, RemoteMouseButton } from './inputProtocol.js'

/** Windows 虚拟键码。直接写字面量：本文件要能在无 Windows 头的环境下参与单测。 */
const VK_LEFT_WIN = 0x5b
const VK_RIGHT_WIN = 0x5c
const VK_L = 0x4c

/** 静默这么久还有键按住，就全抬起。人不在电脑旁，没法自己解。 */
export const REMOTE_INPUT_SILENCE_TIMEOUT_MS = 5000
/** 可靠通道允许的最大跳号。超了说明中间的抬起包多半丢了。 */
export const REMOTE_INPUT_MAX_SEQUENCE_GAP = 1

export interface RemoteInputSink {
  /** 坐标是相对被采集屏幕的归一化值 [0,1]。 */
  moveTo(x: number, y: number): void
  mouseButton(button: RemoteMouseButton, pressed: boolean, x: number, y: number): void
  wheel(delta: number, x: number, y: number): void
  key(keyCode: number, pressed: boolean): void
  text(value: string): void
}

export interface RemoteInputInjector {
  beginSession(sessionId: string): void
  endSession(): void
  isSessionActive(): boolean
  /** 返回 false 表示整包被拒（无会话 / 会话 ID 不符 / 过期包）。 */
  handlePacket(packet: RemoteInputPacket, channel: RemoteInputChannel, nowMs: number): boolean
  /** 由定时器驱动。静默超时且仍有键按住时全部抬起。 */
  tickWatchdog(nowMs: number): void
  heldKeys(): number[]
  heldButtons(): RemoteMouseButton[]
  hasAnythingHeld(): boolean
}

/**
 * Win+L 锁屏一律拦掉。
 *
 * 锁屏之后被控端就再也回不来了——屏幕共享继续跑但画面是锁屏界面，
 * 而解锁需要人站在那台电脑前。远程操作里这是不可逆的单向门。
 */
export function isBlockedKeyCombination(keyCode: number, heldKeys: ReadonlySet<number>): boolean {
  if (keyCode !== VK_L) return false
  return heldKeys.has(VK_LEFT_WIN) || heldKeys.has(VK_RIGHT_WIN)
}

export function createRemoteInputInjector(sink: RemoteInputSink): RemoteInputInjector {
  let sessionId = ''
  let sessionActive = false
  let hasUnreliableSequence = false
  let lastUnreliableSequence = 0
  let hasReliableSequence = false
  let lastReliableSequence = 0
  let lastPacketMs = 0
  const heldKeys = new Set<number>()
  const heldButtons = new Set<RemoteMouseButton>()

  function releaseAllHeld(): void {
    // 先拷贝再清空：sink 理论上可能反过来动到这两个集合。
    const keys = [...heldKeys]
    const buttons = [...heldButtons]
    heldKeys.clear()
    heldButtons.clear()
    for (const keyCode of keys) sink.key(keyCode, false)
    // 抬起位置无所谓，按下时已经在目标位置了；这里只求把键放开。
    for (const button of buttons) sink.mouseButton(button, false, 0, 0)
  }

  function applyEvent(event: RemoteInputPacket['events'][number]): void {
    switch (event.type) {
      case 'mouseMove':
        sink.moveTo(event.x, event.y)
        break
      case 'mouseButton':
        sink.mouseButton(event.button, event.pressed, event.x, event.y)
        if (event.pressed) heldButtons.add(event.button)
        else heldButtons.delete(event.button)
        break
      case 'mouseWheel':
        sink.wheel(event.wheelDelta, event.x, event.y)
        break
      case 'key':
        // 按下被拦时连抬起一并吞掉：只拦按下会让控制端以为键还按着，
        // 而本地根本没按下去，两端状态就对不上了。
        if (isBlockedKeyCombination(event.keyCode, heldKeys)) return
        sink.key(event.keyCode, event.pressed)
        if (event.pressed) heldKeys.add(event.keyCode)
        else heldKeys.delete(event.keyCode)
        break
      case 'text':
        sink.text(event.text)
        break
      case 'releaseAll':
        releaseAllHeld()
        break
    }
  }

  return {
    beginSession(nextSessionId: string): void {
      // 新会话开始前先清干净：上一场若是异常结束，可能还留着按住的键。
      releaseAllHeld()
      sessionId = nextSessionId
      sessionActive = true
      hasUnreliableSequence = false
      lastUnreliableSequence = 0
      hasReliableSequence = false
      lastReliableSequence = 0
      lastPacketMs = 0
    },

    endSession(): void {
      // 会话结束一律把按住的东西全抬起来。控制端崩了、网断了、超时了，
      // 被控端都不该留着 Ctrl 按住的状态。
      releaseAllHeld()
      sessionActive = false
      sessionId = ''
    },

    isSessionActive: () => sessionActive,

    handlePacket(packet, channel, nowMs): boolean {
      if (!sessionActive) return false
      // 会话 ID 不符一律拒收：上一场会话的残留包不该操作这一场的电脑。
      if (packet.sessionId !== sessionId) return false

      if (channel === 'unreliable') {
        // 乱序到达是常态。旧位置盖掉新位置会让光标来回跳，直接丢弃。
        // 丢包本身不需要兜底——下一包就把位置纠正回来了。
        if (hasUnreliableSequence && packet.sequence <= lastUnreliableSequence) return false
        hasUnreliableSequence = true
        lastUnreliableSequence = packet.sequence
      } else {
        // 可靠有序通道本不该跳号。跳了说明链路真出了问题，中间那些抬起包
        // 多半已经丢了，先把按住的全抬掉再继续，避免留下悬空状态。
        if (
          hasReliableSequence &&
          packet.sequence > lastReliableSequence + REMOTE_INPUT_MAX_SEQUENCE_GAP
        ) {
          releaseAllHeld()
        }
        if (hasReliableSequence && packet.sequence <= lastReliableSequence) return false
        hasReliableSequence = true
        lastReliableSequence = packet.sequence
      }

      lastPacketMs = nowMs
      for (const event of packet.events) applyEvent(event)
      return true
    },

    tickWatchdog(nowMs: number): void {
      if (!sessionActive || heldKeys.size + heldButtons.size === 0) return
      // 收到过包才谈得上静默；lastPacketMs 为 0 说明这一场还没收到任何输入。
      if (lastPacketMs === 0) return
      if (nowMs - lastPacketMs < REMOTE_INPUT_SILENCE_TIMEOUT_MS) return
      releaseAllHeld()
    },

    heldKeys: () => [...heldKeys],
    heldButtons: () => [...heldButtons],
    hasAnythingHeld: () => heldKeys.size + heldButtons.size > 0
  }
}
