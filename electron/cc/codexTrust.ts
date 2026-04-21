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
 * Conservative ready signal before injecting the initial task prompt.
 * We wait until the main Codex chrome is visible.
 */
export function isCodexReadyForPromptInjection(raw: string): boolean {
  const text = normalizeTerminalText(raw).toLowerCase()
  if (shouldAutoAcceptCodexTrustPrompt(raw)) return false
  return text.includes('openai codex')
}
