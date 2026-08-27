import type { Terminal } from '@xterm/xterm'

const IMAGE_EXT_FALLBACK = 'png'
const MAX_IMAGE_SIZE = 15 * 1024 * 1024 // 15 MB guard

function normalizedPlatform(platform?: string): string {
  return (
    platform ??
    (typeof navigator !== 'undefined' ? navigator.platform : '')
  ).toLowerCase()
}

function isMacPlatform(platform?: string): boolean {
  const plat = normalizedPlatform(platform)
  return plat.includes('mac')
}

function isWindowsPlatform(platform?: string): boolean {
  const plat = normalizedPlatform(platform)
  return plat.includes('win')
}

function isProtectedMainTuiPlatform(platform?: string): boolean {
  return isMacPlatform(platform) || isWindowsPlatform(platform)
}

/** Copy the current xterm selection to the system clipboard.
 *  OpenCode owns its TUI selection, so callers may provide the text captured
 *  from its OSC 52 clipboard sequence as a fallback. */
export function copySelection(term: Terminal, fallbackSelection = ''): boolean {
  const selection = term.getSelection() || fallbackSelection
  if (!selection) return false
  try {
    void navigator.clipboard.writeText(selection)
  } catch {
    // older/blocked environments — ignore
  }
  return true
}

interface TerminalMouseEvent {
  button: number
  preventDefault(): void
  stopPropagation(): void
}

/** OpenCode copies its own selection on right mouse down and writes both the
 *  system clipboard and OSC 52. Swallowing the right button to open our menu
 *  therefore strands the selection: xterm holds none of its own while mouse
 *  tracking is on, and the OSC 52 the menu reads is only ever emitted by the
 *  copy we just prevented. Forward the button and let the TUI do the copy —
 *  the menu then opens from the `contextmenu` event with that text.
 *  Safe on platforms where OpenCode copies on select instead: opentui only
 *  starts/clears selections on the left button, so the cache survives. */
export function tuiOwnsRightClickCopy(cli: string): boolean {
  return cli.trim().toLowerCase() === 'opencode'
}

/** Keep a TUI's mouse-tracking mode from consuming either half of a right
 *  click before the Electron terminal can handle its copy/paste menu.
 *  Pass `forwardToTui` for TUIs that implement right-click copy themselves. */
export function interceptTerminalRightMouseEvent(
  event: TerminalMouseEvent,
  forwardToTui = false
): boolean {
  if (event.button !== 2) return false
  if (forwardToTui) return false
  event.preventDefault()
  event.stopPropagation()
  return true
}

/** Decode the payload passed to an xterm OSC 52 handler (`target;base64`). */
export function decodeOsc52ClipboardText(data: string): string | null {
  const separator = data.indexOf(';')
  if (separator < 0) return null
  const payload = data.slice(separator + 1).trim()
  if (!payload || payload === '?') return null

  try {
    const binary = globalThis.atob(payload)
    const bytes = Uint8Array.from(binary, (char) => char.charCodeAt(0))
    return new TextDecoder().decode(bytes)
  } catch {
    return null
  }
}

/** Observe OSC 52 clipboard writes without consuming the sequence. OpenCode
 *  uses this sequence when its own mouse-aware TUI copies a selection. */
export function installOsc52SelectionCapture(
  term: Terminal,
  onSelection: (text: string) => void
): () => void {
  const disposable = term.parser.registerOscHandler(52, (data) => {
    const text = decodeOsc52ClipboardText(data)
    if (text) onSelection(text)
    return false
  })
  return () => disposable.dispose()
}

export interface CopyBindingOptions {
  /**
   * The main Windows/macOS TUI uses Ctrl+C as copy. Consume it even without a
   * selection so xterm never turns the copy attempt into ETX/SIGINT.
   * Repository shell terminals leave this false and retain Ctrl+C interrupt.
   */
  ctrlCAsCopyInMainTui?: boolean
  /** Deterministic platform override for tests. */
  platform?: string
}

/** Install platform copy shortcuts on the given xterm instance. */
export function installCopyBinding(
  term: Terminal,
  options: CopyBindingOptions = {}
): void {
  const mac = isMacPlatform(options.platform)
  const protectMainTuiCtrlC =
    options.ctrlCAsCopyInMainTui === true && isProtectedMainTuiPlatform(options.platform)
  term.attachCustomKeyEventHandler((e) => {
    if (e.type !== 'keydown') return true
    const mainTuiCtrlC =
      protectMainTuiCtrlC &&
      e.ctrlKey &&
      !e.metaKey &&
      !e.altKey &&
      e.code === 'KeyC'
    const copyChord =
      mainTuiCtrlC ||
      (mac
        ? e.metaKey && !e.ctrlKey && !e.altKey && e.code === 'KeyC'
        : e.ctrlKey && e.shiftKey && !e.altKey && e.code === 'KeyC')
    if (!copyChord) return true
    const copied = copySelection(term)
    if (!copied && !mainTuiCtrlC) return true
    e.preventDefault()
    return false
  })
}

export interface PasteHandlerOptions {
  /** Session id used to route input back to the PTY. */
  sessionId: string
  /** Forward text (or an image's saved path) to the PTY. */
  writeInput: (sessionId: string, data: string) => void
  /** Optional large-text paste path (chunked in main process). */
  writePastedText?: (sessionId: string, data: string) => Promise<void> | void
  /** Save a pasted image and return its path on disk. */
  saveImage: (
    data: ArrayBuffer,
    ext: string
  ) => Promise<{ ok: boolean; path?: string; error?: string }>
}

/** Install a `paste` listener on the xterm container that:
 *  - If the clipboard contains an image, saves it to disk and writes the
 *    resulting absolute path into the session as if the user had typed it.
 *  - Otherwise, writes the pasted text into the session.
 *  In both cases it preventDefault's the event so xterm does not also paste
 *  the raw bytes into the PTY.
 *
 *  Returns a teardown function. */
export function installPasteHandler(
  container: HTMLElement,
  options: PasteHandlerOptions
): () => void {
  const { sessionId, writeInput, writePastedText, saveImage } = options

  const handler = (event: Event): void => {
    const e = event as ClipboardEvent
    const cd = e.clipboardData
    if (!cd) return

    // 1) image path — prefer image over text when both exist
    for (const item of Array.from(cd.items)) {
      if (!item.type.startsWith('image/')) continue
      const file = item.getAsFile()
      if (!file) continue
      if (file.size === 0 || file.size > MAX_IMAGE_SIZE) continue
      e.preventDefault()
      e.stopPropagation()
      const rawExt = file.type.split('/')[1] ?? IMAGE_EXT_FALLBACK
      const ext = rawExt.replace(/[^a-z0-9]/gi, '').toLowerCase() || IMAGE_EXT_FALLBACK
      void file
        .arrayBuffer()
        .then((buf) => saveImage(buf, ext))
        .then((res) => {
          if (res.ok && res.path) writeInput(sessionId, res.path)
        })
        .catch(() => {
          /* swallow — a failed paste shouldn't crash the session */
        })
      return
    }

    // 2) text — let xterm not handle it so we avoid double insertion
    const text = cd.getData('text')
    if (text) {
      e.preventDefault()
      e.stopPropagation()
      if (writePastedText) {
        void Promise.resolve(writePastedText(sessionId, text)).catch(() => {
          // Fallback to raw write so paste still works when chunked route fails.
          writeInput(sessionId, text)
        })
      } else {
        writeInput(sessionId, text)
      }
    }
  }

  container.addEventListener('paste', handler, { capture: true })
  return () => {
    container.removeEventListener('paste', handler, { capture: true } as
      | AddEventListenerOptions
      | EventListenerOptions)
  }
}

/** Read the system clipboard (image or text) and forward the result into
 *  the session — matches the semantics of the native `paste` handler but
 *  is driven programmatically (e.g. from a context menu). */
export async function pasteFromClipboard(
  options: PasteHandlerOptions
): Promise<void> {
  const { sessionId, writeInput, writePastedText, saveImage } = options
  const clip = navigator.clipboard as
    | (Clipboard & { read?: () => Promise<ClipboardItems> })
    | undefined

  if (clip && typeof clip.read === 'function') {
    try {
      const items = await clip.read()
      for (const item of items) {
        const imageType = item.types.find((t) => t.startsWith('image/'))
        if (!imageType) continue
        const blob = await item.getType(imageType)
        if (blob.size === 0 || blob.size > MAX_IMAGE_SIZE) return
        const rawExt = imageType.split('/')[1] ?? IMAGE_EXT_FALLBACK
        const ext =
          rawExt.replace(/[^a-z0-9]/gi, '').toLowerCase() || IMAGE_EXT_FALLBACK
        const buf = await blob.arrayBuffer()
        const res = await saveImage(buf, ext)
        if (res.ok && res.path) writeInput(sessionId, res.path)
        return
      }
    } catch {
      // permission denied / unsupported MIME — fall through to text
    }
  }

  try {
    const text = await navigator.clipboard.readText()
    if (!text) return
    if (writePastedText) {
      try {
        await writePastedText(sessionId, text)
      } catch {
        writeInput(sessionId, text)
      }
    } else {
      writeInput(sessionId, text)
    }
  } catch {
    /* ignore */
  }
}
