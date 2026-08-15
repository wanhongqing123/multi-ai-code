// 远程桌面信令的编解码。
//
// 这是与 MaiChat 桌面端的互操作契约，不是我们可以自由设计的格式：MaiChat 是主控端，
// Multi-AI Code 只作被控端，两边必须逐字段对齐。实现对照
// MaiChat/desktop/src/remote/RemoteDesktopSignal.cpp 与 CaptureGeometry.cpp 写成，
// 任何一处改动都必须同步改那边，否则连不上而且不会有明确报错。
//
// 信令借道普通 IM 文本消息：不可见前缀让它即使被误显示也几乎看不见，
// 后面跟一个可读标记便于日志排查。

/** U+2063 INVISIBLE SEPARATOR + U+200B ZERO WIDTH SPACE，与 MaiChat 逐码点一致。 */
export const REMOTE_DESKTOP_SIGNAL_PREFIX = '⁣​[remote-desktop]'

/** 协议版本。版本不认识时按普通消息处理，老客户端不会误解新语义。 */
export const REMOTE_DESKTOP_PROTOCOL_VERSION = 1

/** 与 MaiChat 的 kMaxCaptureGeometryDimension 一致：挡住恶意 JSON 里的超大尺寸。 */
const MAX_GEOMETRY_DIMENSION = 65535
/** 与 MaiChat 的 kMaxCaptureGeometryRevision 一致。 */
const MAX_GEOMETRY_REVISION = 2147483647

export type RemoteDesktopSignalType = 'invite' | 'accept' | 'reject' | 'stop' | 'notice'

const SIGNAL_TYPES: readonly RemoteDesktopSignalType[] = [
  'invite',
  'accept',
  'reject',
  'stop',
  'notice'
]

export const REMOTE_DESKTOP_NOTICE_CODES = {
  secureDesktopEntered: 'secure-desktop-entered',
  secureDesktopLeft: 'secure-desktop-left'
} as const

/**
 * 被控端采集坐标系。captureRect 是 sourceSize 内的像素坐标；远端输入最终必须归一化到
 * sourceSize 而不是编码帧，否则区域采集或编码补边都会产生偏移。
 */
export interface RemoteDesktopCaptureGeometry {
  sourceWidth: number
  sourceHeight: number
  captureX: number
  captureY: number
  captureWidth: number
  captureHeight: number
  /** 当前只有 fit：保持采集区域宽高比，编码画布比例不同时在短边补黑。 */
  contentMode: 'fit'
  revision: number
}

export interface RemoteDesktopSignal {
  type: RemoteDesktopSignalType
  sessionId?: string
  roomId?: string
  authProof?: string
  reason?: string
  noticeCode?: string
  captureGeometry?: RemoteDesktopCaptureGeometry
}

export function isRemoteDesktopSignalText(text: string): boolean {
  return text.startsWith(REMOTE_DESKTOP_SIGNAL_PREFIX)
}

function isExactInteger(value: unknown, minimum: number, maximum: number): value is number {
  return (
    typeof value === 'number' &&
    Number.isFinite(value) &&
    Number.isInteger(value) &&
    value >= minimum &&
    value <= maximum
  )
}

/**
 * 几何信息不合法时返回 undefined，而不是把整条信令判为非法：captureGeometry 是 v1 的
 * 兼容扩展，坏掉的可选对象只影响坐标增强；把一条本来合法的 accept 整体丢弃会让新旧
 * 版本混连时进不了房。
 */
function decodeCaptureGeometry(value: unknown): RemoteDesktopCaptureGeometry | undefined {
  if (typeof value !== 'object' || value === null) return undefined
  const raw = value as Record<string, unknown>

  const sourceWidth = raw.sourceWidth
  const sourceHeight = raw.sourceHeight
  const captureX = raw.captureX
  const captureY = raw.captureY
  const captureWidth = raw.captureWidth
  const captureHeight = raw.captureHeight
  const revision = raw.revision

  if (
    !isExactInteger(sourceWidth, 1, MAX_GEOMETRY_DIMENSION) ||
    !isExactInteger(sourceHeight, 1, MAX_GEOMETRY_DIMENSION) ||
    !isExactInteger(captureX, 0, MAX_GEOMETRY_DIMENSION) ||
    !isExactInteger(captureY, 0, MAX_GEOMETRY_DIMENSION) ||
    !isExactInteger(captureWidth, 1, MAX_GEOMETRY_DIMENSION) ||
    !isExactInteger(captureHeight, 1, MAX_GEOMETRY_DIMENSION) ||
    !isExactInteger(revision, 1, MAX_GEOMETRY_REVISION)
  ) {
    return undefined
  }
  // 采集区必须完整落在源画面内。相加前已各自限幅，不会溢出。
  if (captureX + captureWidth > sourceWidth || captureY + captureHeight > sourceHeight) {
    return undefined
  }
  if (raw.contentMode !== 'fit') return undefined

  return {
    sourceWidth,
    sourceHeight,
    captureX,
    captureY,
    captureWidth,
    captureHeight,
    contentMode: 'fit',
    revision
  }
}

export function encodeRemoteDesktopSignal(signal: RemoteDesktopSignal): string {
  const payload: Record<string, unknown> = {
    v: REMOTE_DESKTOP_PROTOCOL_VERSION,
    type: signal.type
  }
  // 空字段一律省略，与 MaiChat 的编码保持一致，避免对端把空串当成有意义的值。
  if (signal.sessionId) payload.sessionId = signal.sessionId
  if (signal.roomId) payload.roomId = signal.roomId
  if (signal.authProof) payload.authProof = signal.authProof
  if (signal.reason) payload.reason = signal.reason
  if (signal.noticeCode) payload.noticeCode = signal.noticeCode
  // 只有 accept 携带几何：它描述的是被控端实际采集了什么，别的信令带上没有意义。
  if (signal.type === 'accept' && signal.captureGeometry) {
    payload.captureGeometry = { ...signal.captureGeometry }
  }
  return REMOTE_DESKTOP_SIGNAL_PREFIX + JSON.stringify(payload)
}

/** 不是信令、版本不认识、或类型未知时返回 null——调用方应把这类文本当普通消息。 */
export function decodeRemoteDesktopSignal(text: string): RemoteDesktopSignal | null {
  if (!isRemoteDesktopSignalText(text)) return null

  let parsed: unknown
  try {
    parsed = JSON.parse(text.slice(REMOTE_DESKTOP_SIGNAL_PREFIX.length))
  } catch {
    return null
  }
  if (typeof parsed !== 'object' || parsed === null || Array.isArray(parsed)) return null

  const raw = parsed as Record<string, unknown>
  if (raw.v !== REMOTE_DESKTOP_PROTOCOL_VERSION) return null

  const type = raw.type
  if (typeof type !== 'string' || !SIGNAL_TYPES.includes(type as RemoteDesktopSignalType)) {
    return null
  }

  const noticeCode = typeof raw.noticeCode === 'string' ? raw.noticeCode : ''
  // 没带 code 的 notice 没有任何意义：对端收到也不知道该显示什么，直接丢弃。
  if (type === 'notice' && !noticeCode) return null

  const signal: RemoteDesktopSignal = { type: type as RemoteDesktopSignalType }
  if (typeof raw.sessionId === 'string' && raw.sessionId) signal.sessionId = raw.sessionId
  if (typeof raw.roomId === 'string' && raw.roomId) signal.roomId = raw.roomId
  if (typeof raw.authProof === 'string' && raw.authProof) signal.authProof = raw.authProof
  if (typeof raw.reason === 'string' && raw.reason) signal.reason = raw.reason
  if (noticeCode) signal.noticeCode = noticeCode
  if (type === 'accept') {
    const geometry = decodeCaptureGeometry(raw.captureGeometry)
    if (geometry) signal.captureGeometry = geometry
  }
  return signal
}
