// 远程输入协议：控制端 → 被控端的鼠标/键盘事件解码。
//
// 线格式由 MaiChat 定义（MaiChat/desktop/src/remote/RemoteInputProtocol.cpp），
// 这里是逐字段对齐的 TS 实现。**改动必须两端同步**，否则表现为"能连上但按键
// 没反应"这种最难查的故障。
//
// 走 TRTC sendCustomCmdMsg，按可靠性分两条通道：
//   cmdID 2 不可靠不有序 —— 鼠标移动，丢了下一包就纠正回来
//   cmdID 3 可靠有序     —— 按键/滚轮/文本，丢一条会留下"按下没抬起"的悬空状态
// 同一 cmdID 内可靠性必须前后一致（TRTC 硬约束），所以只能拆成两个 ID。
//
// 坐标一律归一化到 [0,1]（相对被采集的那块屏幕），不传像素：两端分辨率、
// DPI 缩放、控制端窗口大小都可以不一致。
//
// 本文件只做解码，不碰传输也不碰注入——被控端不发输入，所以没有编码器。

export const REMOTE_INPUT_PROTOCOL_VERSION = 1

/** 鼠标移动走这条：不可靠、不有序。 */
export const REMOTE_INPUT_CMD_ID_UNRELIABLE = 2
/** 按键 / 滚轮 / 文本走这条：可靠、有序。 */
export const REMOTE_INPUT_CMD_ID_RELIABLE = 3

export type RemoteInputChannel = 'unreliable' | 'reliable'

export function channelForCmdId(cmdId: number): RemoteInputChannel | null {
  if (cmdId === REMOTE_INPUT_CMD_ID_UNRELIABLE) return 'unreliable'
  if (cmdId === REMOTE_INPUT_CMD_ID_RELIABLE) return 'reliable'
  return null
}

export type RemoteMouseButton = 'left' | 'right' | 'middle'

export type RemoteInputEvent =
  | { type: 'mouseMove'; x: number; y: number }
  | { type: 'mouseButton'; x: number; y: number; button: RemoteMouseButton; pressed: boolean }
  | { type: 'mouseWheel'; x: number; y: number; wheelDelta: number }
  | { type: 'key'; keyCode: number; pressed: boolean }
  | { type: 'text'; text: string }
  | { type: 'releaseAll' }

export interface RemoteInputPacket {
  protocolVersion: number
  /** 绑定会话：被控端据此拒收上一场会话的残留输入。 */
  sessionId: string
  sequence: number
  events: RemoteInputEvent[]
}

const MAX_SEQUENCE = 0xffffffff
const MAX_KEY_CODE = 0xffff

/** 坐标钳到 [0,1]。坏包不该把光标甩到屏幕外；NaN 显式落到 0。 */
export function clampNormalized(value: unknown): number {
  if (typeof value !== 'number' || Number.isNaN(value)) return 0
  if (value < 0) return 0
  if (value > 1) return 1
  return value
}

/** 与 Qt 的 toDouble() 对齐：非数字读作 0，而不是让整个事件报废。 */
function readNumber(value: unknown): number {
  return typeof value === 'number' && Number.isFinite(value) ? value : 0
}

/** 与 Qt 的 toBool() 对齐：非布尔读作 false。 */
function readBool(value: unknown): boolean {
  return value === true
}

/** 未知按键编号一律落回左键：宁可点错一个键，也不要漏掉配对的抬起留下悬空状态。 */
function buttonFromInt(value: unknown): RemoteMouseButton {
  const index = readNumber(value)
  if (index === 1) return 'right'
  if (index === 2) return 'middle'
  return 'left'
}

/** 解不出来的事件返回 null，由调用方跳过——一条坏事件不该让整包报废。 */
function decodeEvent(raw: unknown): RemoteInputEvent | null {
  if (!raw || typeof raw !== 'object') return null
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
        pressed: readBool(object.d)
      }
    case 'w':
      return {
        type: 'mouseWheel',
        x: clampNormalized(object.x),
        y: clampNormalized(object.y),
        // 一格 120，正数向上。截断而不是四舍五入，与 Qt 的 toInt() 一致。
        wheelDelta: Math.trunc(readNumber(object.w))
      }
    case 'k': {
      const code = Math.trunc(readNumber(object.k))
      // 键码越界的包按坏包处理：拿它去 SendInput 会点到完全无关的键。
      if (code < 0 || code > MAX_KEY_CODE) return null
      return { type: 'key', keyCode: code, pressed: readBool(object.d) }
    }
    case 'x': {
      // 事件级的 s 是文本；根级的 s 是会话 ID。同名不同层，别看混了。
      const text = typeof object.s === 'string' ? object.s : ''
      // 空文本没有注入意义，直接当无效事件丢掉。
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
 * 解析一个输入包。任何无法可靠解析的输入都返回 null。
 *
 * 这个函数直接吃网络来的字节，必须对畸形输入免疫：坏包一律整包丢弃，
 * 绝不半解析后驱动注入——那等于拿残缺的语义去操作别人的电脑。
 */
export function decodeRemoteInputPacket(payload: string): RemoteInputPacket | null {
  if (!payload) return null

  let root: unknown
  try {
    root = JSON.parse(payload)
  } catch {
    return null
  }
  if (!root || typeof root !== 'object' || Array.isArray(root)) return null
  const object = root as Record<string, unknown>

  // 版本对不上一律整包丢弃：宁可不动，也不要用错误的语义去操作别人的电脑。
  if (typeof object.v !== 'number' || Math.trunc(object.v) !== REMOTE_INPUT_PROTOCOL_VERSION) {
    return null
  }

  if (typeof object.n !== 'number' || !Number.isFinite(object.n)) return null
  const sequence = Math.trunc(object.n)
  if (sequence < 0 || sequence > MAX_SEQUENCE) return null

  if (!Array.isArray(object.e)) return null

  const events: RemoteInputEvent[] = []
  for (const item of object.e) {
    const event = decodeEvent(item)
    if (event) events.push(event)
    }

  return {
    protocolVersion: REMOTE_INPUT_PROTOCOL_VERSION,
    sessionId: typeof object.s === 'string' ? object.s : '',
    sequence,
    events
  }
}
