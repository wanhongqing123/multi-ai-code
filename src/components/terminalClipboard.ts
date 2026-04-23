import type { Terminal } from '@xterm/xterm'

const IMAGE_EXT_FALLBACK = 'png'
const MAX_IMAGE_SIZE = 15 * 1024 * 1024 // 15 MB guard

function isMacPlatform(): boolean {
  const plat =
    typeof navigator !== 'undefined' ? navigator.platform.toLowerCase() : ''
  return plat.includes('mac')
}

/** Copy the current xterm selection to the system clipboard.
 *  No-op when there is no selection. */
export function copySelection(term: Terminal): boolean {
  const selection = term.getSelection()
  if (!selection) return false
  try {
    void navigator.clipboard.writeText(selection)
  } catch {
    // older/blocked environments — ignore
  }
  return true
}

/** Install Cmd+C / Ctrl+Shift+C copy on the given xterm instance.
 *  Only consumes the chord when there is a selection — otherwise Ctrl+C
 *  is left alone so it can reach the PTY as SIGINT. */
export function installCopyBinding(term: Terminal): void {
  const mac = isMacPlatform()
  term.attachCustomKeyEventHandler((e) => {
    if (e.type !== 'keydown') return true
    const copyChord = mac
      ? e.metaKey && !e.ctrlKey && !e.altKey && e.code === 'KeyC'
      : e.ctrlKey && e.shiftKey && !e.altKey && e.code === 'KeyC'
    if (!copyChord) return true
    if (!copySelection(term)) return true // nothing to copy — don't swallow Ctrl+C
    e.preventDefault()
    return false
  })
}

export interface PasteHandlerOptions {
  /** Session id used to route input back to the PTY. */
  sessionId: string
  /** Forward text (or an image's saved path) to the PTY. */
  writeInput: (sessionId: string, data: string) => void
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
  const { sessionId, writeInput, saveImage } = options

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
      writeInput(sessionId, text)
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
  const { sessionId, writeInput, saveImage } = options
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
    if (text) writeInput(sessionId, text)
  } catch {
    /* ignore */
  }
}
