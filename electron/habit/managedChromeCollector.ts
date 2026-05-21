import type { RecordHabitEventInput } from './collector.js'

export interface ClickSignal {
  tabId: string
  url: string
  role?: string
  label?: string
  selectorHint?: string
}

export interface InputSignal {
  tabId: string
  url: string
  type?: string
  label?: string
  placeholder?: string
  role?: string
  value?: string
}

function truncate(text: string, max = 120): string {
  const trimmed = text.trim()
  if (trimmed.length <= max) return trimmed
  return `${trimmed.slice(0, max - 1)}…`
}

function safeUrlParts(url: string): { origin: string; path: string } {
  try {
    const parsed = new URL(url)
    return {
      origin: parsed.origin,
      path: parsed.pathname || '/'
    }
  } catch {
    return {
      origin: url,
      path: '/'
    }
  }
}

function elementHint(parts: Array<string | undefined>): string {
  const first = parts.find((value) => typeof value === 'string' && value.trim().length > 0)
  return truncate(first ?? 'interaction')
}

export function buildSiteVisitEvent(url: string, tabId: string): RecordHabitEventInput {
  const parts = safeUrlParts(url)
  return {
    kind: 'site_visit',
    source: 'managed_chrome',
    text: `Visit ${parts.origin}${parts.path}`,
    extras: {
      actionType: 'navigate',
      tabId,
      url,
      origin: parts.origin,
      path: parts.path
    }
  }
}

export function buildSiteClickEvent(input: ClickSignal): RecordHabitEventInput {
  const parts = safeUrlParts(input.url)
  const hint = elementHint([input.label, input.role, input.selectorHint])
  return {
    kind: 'site_click',
    source: 'managed_chrome',
    text: `Click ${hint}`,
    extras: {
      actionType: 'click',
      tabId: input.tabId,
      url: input.url,
      origin: parts.origin,
      path: parts.path,
      role: input.role ?? null,
      selectorHint: input.selectorHint ?? null,
      elementHint: hint
    }
  }
}

export function buildSiteInputHintEvent(input: InputSignal): RecordHabitEventInput | null {
  const inputType = input.type?.trim().toLowerCase() ?? 'text'
  if (inputType === 'password') return null

  const parts = safeUrlParts(input.url)
  const hint = elementHint([input.label, input.placeholder, input.role, inputType])
  return {
    kind: 'site_input_hint',
    source: 'managed_chrome',
    text: `Input ${hint}`,
    extras: {
      actionType: 'input',
      tabId: input.tabId,
      url: input.url,
      origin: parts.origin,
      path: parts.path,
      inputType,
      elementHint: hint
    }
  }
}
