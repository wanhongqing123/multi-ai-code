export type Theme = 'light' | 'dark'

export const THEME_CHANGE_EVENT = 'mac-theme-change'

const STORAGE_KEY = 'mac.theme'
const DARK_CLASS = 'theme-dark'

function isValid(v: string | null): v is Theme {
  return v === 'light' || v === 'dark'
}

function systemPrefersDark(): boolean {
  return typeof matchMedia === 'function' && matchMedia('(prefers-color-scheme: dark)').matches
}

function dispatchThemeChange(theme: Theme): void {
  if (typeof window !== 'undefined' && typeof CustomEvent === 'function') {
    window.dispatchEvent(new CustomEvent<Theme>(THEME_CHANGE_EVENT, { detail: theme }))
  }
}

export function getTheme(): Theme {
  const stored = localStorage.getItem(STORAGE_KEY)
  if (isValid(stored)) return stored
  return systemPrefersDark() ? 'dark' : 'light'
}

export function setTheme(theme: Theme): void {
  localStorage.setItem(STORAGE_KEY, theme)
  const root = document.documentElement
  if (theme === 'dark') root.classList.add(DARK_CLASS)
  else root.classList.remove(DARK_CLASS)
  dispatchThemeChange(theme)
}

export function toggleTheme(): Theme {
  const next: Theme = getTheme() === 'dark' ? 'light' : 'dark'
  setTheme(next)
  return next
}

export function applyTheme(): void {
  const theme = getTheme()
  const root = document.documentElement
  if (theme === 'dark') root.classList.add(DARK_CLASS)
  else root.classList.remove(DARK_CLASS)
  dispatchThemeChange(theme)
}
