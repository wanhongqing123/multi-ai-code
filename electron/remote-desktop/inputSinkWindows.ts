// 真正把输入落到系统的那一层（Windows）。
//
// 这里刻意保持极薄且不含任何判定——所有会话绑定、悬空键兜底、危险组合键
// 拦截都在 inputInjector.ts，那边可以用假 sink 完整单测。本文件测不了
// （要真的动别人的鼠标），所以它必须薄到一眼能看完。
//
// 走 koffi 直接调 user32.dll：Electron 的 webContents.sendInputEvent 只能
// 注入到自己的窗口，到不了操作系统。koffi 是 N-API 预编译的，不需要
// node-gyp——这台机器上 node-gyp 找不到 VS，编译方案一律走不通。

import type { RemoteInputSink } from './inputInjector.js'
import type { RemoteMouseButton } from './inputProtocol.js'

/** SendInput 的 cbSize 必须正好等于 sizeof(INPUT)：x64 下是 40，对不上直接返回 0 且不报错。 */
const INPUT_SIZE_X64 = 40

const INPUT_MOUSE = 0
const INPUT_KEYBOARD = 1

const MOUSEEVENTF_MOVE = 0x0001
const MOUSEEVENTF_LEFTDOWN = 0x0002
const MOUSEEVENTF_LEFTUP = 0x0004
const MOUSEEVENTF_RIGHTDOWN = 0x0008
const MOUSEEVENTF_RIGHTUP = 0x0010
const MOUSEEVENTF_MIDDLEDOWN = 0x0020
const MOUSEEVENTF_MIDDLEUP = 0x0040
const MOUSEEVENTF_WHEEL = 0x0800
const MOUSEEVENTF_ABSOLUTE = 0x8000
/** 绝对坐标按整个虚拟桌面算，不是主屏——多显示器下少了它会全跑偏。 */
const MOUSEEVENTF_VIRTUALDESK = 0x4000

const KEYEVENTF_KEYUP = 0x0002
const KEYEVENTF_UNICODE = 0x0004

const SM_XVIRTUALSCREEN = 76
const SM_YVIRTUALSCREEN = 77
const SM_CXVIRTUALSCREEN = 78
const SM_CYVIRTUALSCREEN = 79

/** 被采集屏幕在虚拟桌面里的位置和大小。归一化坐标是相对它算的。 */
export interface CaptureScreenRect {
  left: number
  top: number
  width: number
  height: number
}

/**
 * 归一化坐标 → SendInput 的绝对坐标（0..65535，相对整个虚拟桌面）。
 *
 * 两步换算：先把 [0,1] 映射回被采集屏幕内的桌面像素，再换算成虚拟桌面的
 * 归一化刻度。少了第一步，副屏共享时光标会落到主屏上。
 */
export function toAbsoluteCoordinates(
  x: number,
  y: number,
  capture: CaptureScreenRect,
  virtualScreen: CaptureScreenRect
): { absX: number; absY: number } {
  const pixelX = capture.left + x * capture.width
  const pixelY = capture.top + y * capture.height
  // 分母用 width-1：65535 对应最后一个像素，不是像素数。
  const spanX = Math.max(1, virtualScreen.width - 1)
  const spanY = Math.max(1, virtualScreen.height - 1)
  const absX = Math.round(((pixelX - virtualScreen.left) * 65535) / spanX)
  const absY = Math.round(((pixelY - virtualScreen.top) * 65535) / spanY)
  return {
    absX: Math.min(65535, Math.max(0, absX)),
    absY: Math.min(65535, Math.max(0, absY))
  }
}

function mouseButtonFlags(button: RemoteMouseButton, pressed: boolean): number {
  if (button === 'right') return pressed ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP
  if (button === 'middle') return pressed ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP
  return pressed ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP
}

interface Koffi {
  load(name: string): { func(signature: string): (...args: unknown[]) => number }
  struct(name: string, fields: Record<string, unknown>): unknown
  sizeof(type: unknown): number
}

export interface WindowsInputSink extends RemoteInputSink {
  /** 被采集屏幕变了要重新告知，否则坐标换算继续按旧屏算，整体偏移。 */
  setCaptureScreen(rect: CaptureScreenRect): void
}

/**
 * 创建 Windows 输入 sink。调用方负责只在 Windows 上创建它。
 *
 * koffi 用动态 require：没开远程控制的用户不该为它付加载成本，
 * 也不该因为它加载失败就连屏幕共享都用不了。
 */
export function createWindowsInputSink(
  koffi: Koffi,
  logger?: (message: string, detail?: Record<string, unknown>) => void
): WindowsInputSink {
  const user32 = koffi.load('user32.dll')

  const MOUSEINPUT = koffi.struct('MOUSEINPUT', {
    dx: 'int32',
    dy: 'int32',
    mouseData: 'uint32',
    dwFlags: 'uint32',
    time: 'uint32',
    dwExtraInfo: 'uintptr'
  })
  const KEYBDINPUT = koffi.struct('KEYBDINPUT', {
    wVk: 'uint16',
    wScan: 'uint16',
    dwFlags: 'uint32',
    time: 'uint32',
    dwExtraInfo: 'uintptr',
    // 补足到 MOUSEINPUT 的 32 字节：INPUT 是联合体，两种事件必须同宽，
    // 否则 cbSize 对不上，SendInput 静默返回 0。
    _pad: 'uint64'
  })
  const MOUSE_INPUT = koffi.struct('INPUT_MOUSE', { type: 'uint32', _pad: 'uint32', mi: MOUSEINPUT })
  const KEY_INPUT = koffi.struct('INPUT_KEYBOARD', { type: 'uint32', _pad: 'uint32', ki: KEYBDINPUT })

  const mouseSize = koffi.sizeof(MOUSE_INPUT)
  const keySize = koffi.sizeof(KEY_INPUT)
  if (mouseSize !== INPUT_SIZE_X64 || keySize !== INPUT_SIZE_X64) {
    throw new Error(`INPUT 结构体大小异常：mouse=${mouseSize} key=${keySize}，应为 ${INPUT_SIZE_X64}`)
  }

  const SendInputMouse = user32.func('uint32 __stdcall SendInput(uint32 c, INPUT_MOUSE *i, int32 size)')
  const SendInputKey = user32.func('uint32 __stdcall SendInput(uint32 c, INPUT_KEYBOARD *i, int32 size)')
  const GetSystemMetrics = user32.func('int __stdcall GetSystemMetrics(int index)')

  function virtualScreen(): CaptureScreenRect {
    return {
      left: GetSystemMetrics(SM_XVIRTUALSCREEN),
      top: GetSystemMetrics(SM_YVIRTUALSCREEN),
      width: GetSystemMetrics(SM_CXVIRTUALSCREEN),
      height: GetSystemMetrics(SM_CYVIRTUALSCREEN)
    }
  }

  // 默认按整个虚拟桌面算。真实采集屏幕由 setCaptureScreen 覆盖。
  let capture: CaptureScreenRect = virtualScreen()

  function sendMouse(flags: number, absX: number, absY: number, mouseData = 0): void {
    const sent = SendInputMouse(
      1,
      [{ type: INPUT_MOUSE, _pad: 0, mi: { dx: absX, dy: absY, mouseData, dwFlags: flags, time: 0, dwExtraInfo: 0 } }],
      mouseSize
    )
    // 返回 0 = 一个事件都没注入。多半是被 UIPI 挡了（对方进程权限更高），
    // 静默失败会表现为"远程点了没反应"，必须记下来。
    if (sent !== 1) logger?.('remote-input: SendInput(mouse) rejected', { flags, sent })
  }

  function sendKey(wVk: number, wScan: number, flags: number): void {
    const sent = SendInputKey(
      1,
      [{ type: INPUT_KEYBOARD, _pad: 0, ki: { wVk, wScan, dwFlags: flags, time: 0, dwExtraInfo: 0, _pad: 0 } }],
      keySize
    )
    if (sent !== 1) logger?.('remote-input: SendInput(key) rejected', { wVk, flags, sent })
  }

  function moveAbsolute(x: number, y: number): { absX: number; absY: number } {
    const { absX, absY } = toAbsoluteCoordinates(x, y, capture, virtualScreen())
    sendMouse(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK, absX, absY)
    return { absX, absY }
  }

  return {
    setCaptureScreen(rect: CaptureScreenRect): void {
      capture = rect
      logger?.('remote-input: capture screen set', { ...rect })
    },

    moveTo(x, y): void {
      moveAbsolute(x, y)
    },

    mouseButton(button, pressed, x, y): void {
      // 先移到位再按：SendInput 的按下事件用的是当前光标位置，
      // 不先移动会点在上一次的位置上。
      const { absX, absY } = moveAbsolute(x, y)
      sendMouse(mouseButtonFlags(button, pressed) | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK, absX, absY)
    },

    wheel(delta, x, y): void {
      const { absX, absY } = moveAbsolute(x, y)
      sendMouse(MOUSEEVENTF_WHEEL | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK, absX, absY, delta)
    },

    key(keyCode, pressed): void {
      sendKey(keyCode, 0, pressed ? 0 : KEYEVENTF_KEYUP)
    },

    text(value): void {
      // 走 KEYEVENTF_UNICODE 直接送码位，绕开键盘布局——中文、emoji 和
      // 手机软键盘的输出都不可能用虚拟键码表示。
      for (const char of value) {
        const code = char.codePointAt(0)
        if (code === undefined) continue
        // BMP 之外要拆成代理对分别发送。
        const units =
          code > 0xffff
            ? [0xd800 + ((code - 0x10000) >> 10), 0xdc00 + ((code - 0x10000) & 0x3ff)]
            : [code]
        for (const unit of units) {
          sendKey(0, unit, KEYEVENTF_UNICODE)
          sendKey(0, unit, KEYEVENTF_UNICODE | KEYEVENTF_KEYUP)
        }
      }
    }
  }
}
