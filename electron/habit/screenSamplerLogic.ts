/**
 * Pure logic for the screen sampler — the bits that don't touch electron or
 * the filesystem. Kept separate so they can be unit tested
 * without spinning up a window or stubbing native modules.
 */

/** Active-window info we care about for filtering and dedup. */
export interface WindowSample {
  /** Visible window title (may contain user data, treat as sensitive). */
  title: string
  /** Process name or app display name. */
  appName: string
  /** macOS bundle id; undefined elsewhere. */
  bundleId?: string
  /** PID, for diagnostics. */
  processId?: number
}

export type BlocklistRule = string

/**
 * Default privacy blocklist. Window titles or app names that match any
 * substring (case-insensitive) are dropped before persistence. The list is
 * intentionally short and obvious; users can add more in settings.
 */
export const DEFAULT_APP_BLOCKLIST: BlocklistRule[] = [
  '1password',
  'bitwarden',
  'lastpass',
  'keepass',
  // Browser "private / incognito / inprivate" window markers
  '隐私浏览',
  'incognito',
  'inprivate',
  'private browsing',
  'private window'
]

/**
 * Patterns that get masked inside any captured window-title text. None of
 * these match the title as a whole — they only redact the matched span.
 * The replacement is a fixed marker so the row remains useful for
 * deduplication ("same template + redacted secret" still groups).
 */
const SECRET_PATTERNS: RegExp[] = [
  /sk-[A-Za-z0-9_-]{16,}/g,
  /Bearer\s+[A-Za-z0-9._-]{16,}/gi,
  // Generic high-entropy long alnum string (40+ chars) — keep loose so it
  // catches arbitrary api keys; safer to over-mask than under-mask here.
  /\b[A-Za-z0-9+/=_-]{40,}\b/g,
  // GitHub token prefixes
  /\bghp_[A-Za-z0-9]{20,}\b/g,
  /\bgho_[A-Za-z0-9]{20,}\b/g,
  /\bgithub_pat_[A-Za-z0-9_]{20,}\b/g
]

/** Masks any secret-shaped substring in `text` with a fixed marker. */
export function redactSecrets(text: string): string {
  if (!text) return text
  let out = text
  for (const re of SECRET_PATTERNS) out = out.replace(re, '<redacted>')
  return out
}

/**
 * Decides whether a sample should be persisted at all. Returns:
 *   - null  → drop (matched blocklist)
 *   - sample (possibly with title rewritten) → keep
 *
 * Matching is case-insensitive substring over title + appName + bundleId.
 */
export function applyBlocklist(
  sample: WindowSample,
  blocklist: BlocklistRule[]
): WindowSample | null {
  const haystacks = [sample.title, sample.appName, sample.bundleId ?? '']
    .map((s) => s.toLowerCase())
  for (const rule of blocklist) {
    const r = rule.toLowerCase()
    if (!r) continue
    if (haystacks.some((h) => h.includes(r))) return null
  }
  return {
    ...sample,
    title: redactSecrets(sample.title)
  }
}

/**
 * In-process dedupe: same window title within `windowMs` is considered the
 * same observation and the duplicate is dropped. Returns `true` when this
 * sample should be persisted, `false` when it's a duplicate. The cache
 * keeps at most `maxKeys` entries so a very chatty title-change stream
 * doesn't grow unbounded.
 */
export class SampleDedupe {
  private readonly recent = new Map<string, number>()
  private readonly windowMs: number
  private readonly maxKeys: number

  constructor(opts: { windowMs?: number; maxKeys?: number } = {}) {
    this.windowMs = opts.windowMs ?? 24 * 60 * 60 * 1000
    this.maxKeys = opts.maxKeys ?? 5000
  }

  shouldKeep(key: string, now: number = Date.now()): boolean {
    if (!key) return false
    const last = this.recent.get(key)
    if (last !== undefined && now - last < this.windowMs) return false
    this.recent.set(key, now)
    if (this.recent.size > this.maxKeys) this.gc(now)
    return true
  }

  /** Drops entries older than `windowMs`. Called opportunistically. */
  private gc(now: number): void {
    const cutoff = now - this.windowMs
    for (const [k, ts] of this.recent) {
      if (ts < cutoff) this.recent.delete(k)
    }
  }

  /** Test helper. */
  _size(): number {
    return this.recent.size
  }
}

/**
 * Dates we use for the `screen-samples/<YYYY-MM-DD>/<file>.png` layout.
 * Exposed so we can match the writer and the cleanup walker.
 */
export function dateFolderFor(ts: number): string {
  const d = new Date(ts)
  const yyyy = d.getFullYear()
  const mm = String(d.getMonth() + 1).padStart(2, '0')
  const dd = String(d.getDate()).padStart(2, '0')
  return `${yyyy}-${mm}-${dd}`
}

/** Filename-safe ISO timestamp (`:` and `.` replaced with `-`). */
export function safeIsoStamp(ts: number): string {
  return new Date(ts).toISOString().replace(/[:.]/g, '-')
}
