// Minimal browser-API polyfills for theme tests running in the node environment.
// localStorage: full Map-backed implementation with getItem/setItem/removeItem/clear
// document.documentElement: minimal classList stub

import { vi } from 'vitest'

// ---- localStorage polyfill ----
const store: Map<string, string> = new Map()
const localStorageMock = {
  getItem: (key: string) => store.get(key) ?? null,
  setItem: (key: string, value: string) => { store.set(key, String(value)) },
  removeItem: (key: string) => { store.delete(key) },
  clear: () => { store.clear() },
  get length() { return store.size },
  key: (n: number) => Array.from(store.keys())[n] ?? null,
}
vi.stubGlobal('localStorage', localStorageMock)

// ---- document / documentElement polyfill ----
const classes = new Set<string>()
const classListMock = {
  add: (...tokens: string[]) => { tokens.forEach((t) => classes.add(t)) },
  remove: (...tokens: string[]) => { tokens.forEach((t) => classes.delete(t)) },
  contains: (token: string) => classes.has(token),
  toggle: (token: string, force?: boolean) => {
    if (force === true || (force === undefined && !classes.has(token))) {
      classes.add(token); return true
    } else {
      classes.delete(token); return false
    }
  },
}
const documentElementMock = { classList: classListMock }
const documentMock = { documentElement: documentElementMock }
vi.stubGlobal('document', documentMock)
