// 远程输入包的解码。
//
// 与信令一样，这是与 MaiChat 的互操作契约，实现对照
// MaiChat/desktop/src/remote/RemoteInputProtocol.cpp 写成。Multi-AI Code 只作被控端，
// 因此这里只需要解码；编码留给主控端。
//
// 输入走 TRTC sendCustomCmdMsg，按可靠性分两条通道：
//   cmdID 2（不可靠）—— 鼠标移动、滚轮：丢了下一包就纠正回来
//   cmdID 3（可靠）  —— 按下/抬起、文本、全抬：丢一条会留下"按住没抬起"的悬空状态
// 两条通道的丢包语义相反，兜底策略不能共用，所以 cmdID 必须一路带到上层。

/** 与 MaiChat 的 kCmdIdUnreliable 一致：鼠标移动 / 滚轮。 */
export const REMOTE_INPUT_CMD_ID_UNRELIABLE = 2
/** 与 MaiChat 的 kCmdIdReliable 一致：按键、文本、全抬。 */
export const REMOTE_INPUT_CMD_ID_RELIABLE = 3
/** 与 MaiChat 的 kProtocolVersion 一致。 */
export const REMOTE_INPUT_PROTOCOL_VERSION = 1

const MAX_SEQUENCE = 0xffffffff
const MAX_KEY_CODE = 0xffff

export type RemoteInputMouseButton = 'left' | 'right' | 'middle'

export type RemoteInputEvent =
  | { type: 'mouseMove'; x: number; y: number }
  | { type: 'mouseButton'; x: number; y: number; button: RemoteInputMouseButton; pressed: boolean }
  | { type: 'mouseWheel'; x: number; y: number; wheelDelta: number }
  | { type: 'key'; keyCode: number; pressed: boolean }
  | { type: 'text'; text: string }
  | { type: 'releaseAll' }

export interface RemoteInputPacket {
  sessionId: string
  sequence: number
  events: RemoteInputEvent[]
}

/**
 * 坐标一律归一化到 [0,1]。NaN 比不出大小，两个分支都不会命中，必须显式挡掉——
 * 否则它会一路传到注入层，把光标打到未定义的位置。
 */
function clampNormalized(value: unknown): number {
  if (typeof value !== 'number' || Number.isNaN(value)) return 0
  if (value < 0) return 0
  if (value > 1) return 1
  return value
}

/** 未知按键编号落回左键：宁可点错一个键，也不要当成"没有按键"而漏掉配对的抬起。 */
function buttonFromInt(value: unknown): RemoteInputMouseButton {
  if (value === 1) return 'right'
  if (value === 2) return 'middle'
  return 'left'
}

/** 解不出来的事件返回 null，由调用方跳过——一条坏事件不该让整包报废。 */
function decodeEvent(raw: unknown): RemoteInputEvent | null {
  if (typeof raw !== 'object' || raw === null || Array.isArray(raw)) return null
  const object = raw as Record<string, unknown>

  switch (object.t) {
    case 'm':
      return { type: 'mouseMove', x: clampNormalized(object.x), y: clampNormalized(object.y) }
    case 'b':
      return {
        type: 'mouseButton',
        x: clampNormalized(object.x),
        y: clampNormalized(object.y),
        button: buttonFromInt(object.b),
        pressed: object.d === true
      }
    case 'w':
      return {
        type: 'mouseWheel',
        x: clampNormalized(object.x),
        y: clampNormalized(object.y),
        wheelDelta: typeof object.w === 'number' && Number.isFinite(object.w) ? Math.trunc(object.w) : 0
      }
    case 'k': {
      const code = typeof object.k === 'number' && Number.isFinite(object.k) ? Math.trunc(object.k) : -1
      // 键码越界按坏事件处理：拿它去注入会点到完全无关的键。
      if (code < 0 || code > MAX_KEY_CODE) return null
      return { type: 'key', keyCode: code, pressed: object.d === true }
    }
    case 'x': {
      const text = typeof object.s === 'string' ? object.s : ''
      // 空文本没有注入意义。
      if (!text) return null
      return { type: 'text', text }
    }
    case 'r':
      return { type: 'releaseAll' }
    default:
      return null
  }
}

/**
 * 整包丢弃 vs 跳过单条，是两个不同层次的容错，必须分清：
 *   整包丢弃 —— 版本不符、序号越界、JSON 坏：宁可不动，也不要用错误的语义操作别人的电脑
 *   跳过单条 —— 未知事件类型、键码越界、空文本：多半来自新版主控端，老被控端应该
 *               跳过它继续执行认得的，否则一个新事件就让整条输入流卡死
 */
export function decodeRemoteInputPacket(payload: string): RemoteInputPacket | null {
  let parsed: unknown
  try {
    parsed = JSON.parse(payload)
  } catch {
    return null
  }
  if (typeof parsed !== 'object' || parsed === null || Array.isArray(parsed)) return null

  const root = parsed as Record<string, unknown>
  if (root.v !== REMOTE_INPUT_PROTOCOL_VERSION) return null

  const sequence = root.n
  if (
    typeof sequence !== 'number' ||
    !Number.isFinite(sequence) ||
    !Number.isInteger(sequence) ||
    sequence < 0 ||
    sequence > MAX_SEQUENCE
  ) {
    return null
  }
  if (!Array.isArray(root.e)) return null

  const events: RemoteInputEvent[] = []
  for (const item of root.e) {
    const event = decodeEvent(item)
    if (event) events.push(event)
  }

  return {
    sessionId: typeof root.s === 'string' ? root.s : '',
    sequence,
    events
  }
}
