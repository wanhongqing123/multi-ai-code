/**
 * Strip common ANSI escape sequences so prompt text can be matched reliably.
 */
export function normalizeTerminalText(raw: string): string {
  return raw
    // OSC: ESC ] ... BEL or ESC \
    .replace(/\u001B\][\s\S]*?(?:\u0007|\u001B\\)/g, ' ')
    // CSI: ESC [ ... command
    .replace(/\u001B\[[0-?]*[ -/]*[@-~]/g, ' ')
    // Fe escape sequences (single-char)
    .replace(/\u001B[@-Z\\-_]/g, ' ')
    .replace(/\r/g, '')
    .replace(/\s+/g, ' ')
    .trim()
}

/**
 * Codex trust gate shown on first run in a directory:
 * "Do you trust the contents of this directory?" + "Press enter to continue"
 */
export function shouldAutoAcceptCodexTrustPrompt(raw: string): boolean {
  const text = normalizeTerminalText(raw).toLowerCase()
  return (
    text.includes('do you trust the contents of this directory?') &&
    text.includes('press enter to continue')
  )
}

/**
 * Session-scoped or per-tool approval prompts shown by Claude Code's TUI
 * that offer a persistent "Yes, and always allow ..." option (typically
 * option 2). Covers:
 * - "Yes, allow all edits during this session (shift+tab)"
 * - "Do you want to proceed? … 2. Yes, and always allow access to X from
 *   this project" — the per-Bash / per-tool permission prompt
 *
 * Both prompts share the literal "and always allow" substring on option 2,
 * which we use as the gate so we only auto-accept when a persistent option
 * is actually offered (and not on weaker per-call prompts).
 */
export function shouldAutoAcceptSessionEditPrompt(raw: string): boolean {
  const text = normalizeTerminalText(raw).toLowerCase()
  // Variant 1 — session-wide edit grant ("Yes, allow all edits during this
  // session"). Option 2 always allows edits for the rest of the session.
  if (text.includes('allow all edits during this session')) return true
  // Variant 2 — per-tool prompt: "Do you want to proceed?" with a
  // persistent "Yes, and always allow ... from this project" option 2.
  if (
    text.includes('do you want to proceed') &&
    text.includes('and always allow')
  ) {
    return true
  }
  return false
}

/**
 * Conservative ready signal before injecting the initial task prompt.
 * We wait until the main Codex chrome is visible.
 */
export function isCodexReadyForPromptInjection(raw: string): boolean {
  const text = normalizeTerminalText(raw).toLowerCase()
  if (shouldAutoAcceptCodexTrustPrompt(raw)) return false
  return text.includes('openai codex')
}

/**
 * Ready signal for Claude Code's TUI. Claude paints an input box and the
 * "? for shortcuts" hint once its prompt is interactive — injecting text
 * before that point gets wiped by the TUI's startup redraw.
 */
export function isClaudeReadyForPromptInjection(raw: string): boolean {
  const text = normalizeTerminalText(raw).toLowerCase()
  return text.includes('? for shortcuts')
}
