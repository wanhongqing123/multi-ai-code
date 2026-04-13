import { EventEmitter } from 'events'

export interface StageDoneMeta {
  raw: string
  params: Record<string, string>
}

export interface FeedbackMeta {
  raw: string
  params: Record<string, string>
}

const ANSI_RE =
  // eslint-disable-next-line no-control-regex
  /[\u001B\u009B][[\]()#;?]*(?:(?:(?:[a-zA-Z0-9]*(?:;[a-zA-Z0-9]*)*)?\u0007)|(?:(?:\d{1,4}(?:;\d{0,4})*)?[0-9A-PR-TZcf-nq-uy=><~]))/g

const DONE_RE = /<<STAGE_DONE([^>]*)>>/
const FEEDBACK_RE = /<<FEEDBACK_TO_STAGE=(\d+)([^>]*)>>/

function stripAnsi(s: string): string {
  return s.replace(ANSI_RE, '')
}

/**
 * Parses a params section like `artifact=foo/bar.md verdict=pass` or
 * `summary="hello world" verdict=pass`.
 */
function parseParams(tail: string): Record<string, string> {
  const out: Record<string, string> = {}
  const re = /(\w+)=(?:"([^"]*)"|'([^']*)'|(\S+))/g
  let m: RegExpExecArray | null
  while ((m = re.exec(tail)) !== null) {
    out[m[1]] = m[2] ?? m[3] ?? m[4] ?? ''
  }
  return out
}

/**
 * Scans a PTY output stream for <<STAGE_DONE ...>> and <<FEEDBACK_TO_STAGE=N ...>>
 * markers. Handles ANSI escapes and chunk boundaries.
 *
 * Events:
 *   'done'     — ({raw, params})
 *   'feedback' — ({targetStage, raw, params})
 */
export class StageDoneScanner extends EventEmitter {
  /** Max bytes we keep in buffer to detect markers spanning chunks. */
  private readonly maxBuffer: number
  private buffer = ''
  private firedDoneAt = -1
  private firedFeedbackAt = -1

  constructor(maxBuffer = 16_384) {
    super()
    this.maxBuffer = maxBuffer
  }

  push(chunk: string): void {
    this.buffer += stripAnsi(chunk)
    if (this.buffer.length > this.maxBuffer) {
      this.buffer = this.buffer.slice(-this.maxBuffer)
      this.firedDoneAt = Math.max(-1, this.firedDoneAt - (this.buffer.length - this.maxBuffer))
      this.firedFeedbackAt = Math.max(
        -1,
        this.firedFeedbackAt - (this.buffer.length - this.maxBuffer)
      )
    }

    // STAGE_DONE
    const doneMatch = DONE_RE.exec(this.buffer)
    if (doneMatch && doneMatch.index > this.firedDoneAt) {
      this.firedDoneAt = doneMatch.index
      this.emit('done', {
        raw: doneMatch[0],
        params: parseParams(doneMatch[1] ?? '')
      } as StageDoneMeta)
    }

    // FEEDBACK_TO_STAGE
    const fbMatch = FEEDBACK_RE.exec(this.buffer)
    if (fbMatch && fbMatch.index > this.firedFeedbackAt) {
      this.firedFeedbackAt = fbMatch.index
      this.emit('feedback', {
        targetStage: Number(fbMatch[1]),
        raw: fbMatch[0],
        params: parseParams(fbMatch[2] ?? '')
      })
    }
  }

  reset(): void {
    this.buffer = ''
    this.firedDoneAt = -1
    this.firedFeedbackAt = -1
  }
}
