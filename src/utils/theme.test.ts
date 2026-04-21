import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest'
import { getTheme, setTheme, toggleTheme, applyTheme } from './theme.js'

describe('theme', () => {
  beforeEach(() => {
    localStorage.clear()
    document.documentElement.classList.remove('theme-dark')
  })
  afterEach(() => {
    localStorage.clear()
    document.documentElement.classList.remove('theme-dark')
  })

  describe('getTheme', () => {
    it('returns "light" when no stored value and system is light', () => {
      vi.stubGlobal('matchMedia', () => ({ matches: false }))
      expect(getTheme()).toBe('light')
    })

    it('returns "dark" when no stored value and system prefers dark', () => {
      vi.stubGlobal('matchMedia', () => ({ matches: true }))
      expect(getTheme()).toBe('dark')
    })

    it('returns stored value when present', () => {
      localStorage.setItem('mac.theme', 'dark')
      expect(getTheme()).toBe('dark')
    })

    it('ignores invalid stored value', () => {
      localStorage.setItem('mac.theme', 'garbage')
      vi.stubGlobal('matchMedia', () => ({ matches: false }))
      expect(getTheme()).toBe('light')
    })
  })

  describe('setTheme', () => {
    it('persists the choice to localStorage', () => {
      setTheme('dark')
      expect(localStorage.getItem('mac.theme')).toBe('dark')
    })

    it('adds theme-dark class when dark', () => {
      setTheme('dark')
      expect(document.documentElement.classList.contains('theme-dark')).toBe(true)
    })

    it('removes theme-dark class when light', () => {
      document.documentElement.classList.add('theme-dark')
      setTheme('light')
      expect(document.documentElement.classList.contains('theme-dark')).toBe(false)
    })
  })

  describe('toggleTheme', () => {
    it('switches from light to dark and back', () => {
      setTheme('light')
      expect(toggleTheme()).toBe('dark')
      expect(localStorage.getItem('mac.theme')).toBe('dark')
      expect(toggleTheme()).toBe('light')
      expect(localStorage.getItem('mac.theme')).toBe('light')
    })
  })

  describe('applyTheme', () => {
    it('applies stored theme on startup', () => {
      localStorage.setItem('mac.theme', 'dark')
      applyTheme()
      expect(document.documentElement.classList.contains('theme-dark')).toBe(true)
    })

    it('does NOT write to localStorage when no stored value and system prefers dark', () => {
      vi.stubGlobal('matchMedia', () => ({ matches: true }))
      applyTheme()
      expect(document.documentElement.classList.contains('theme-dark')).toBe(true)
      expect(localStorage.getItem('mac.theme')).toBeNull()
    })

    it('does NOT write to localStorage when no stored value and system is light', () => {
      vi.stubGlobal('matchMedia', () => ({ matches: false }))
      applyTheme()
      expect(document.documentElement.classList.contains('theme-dark')).toBe(false)
      expect(localStorage.getItem('mac.theme')).toBeNull()
    })
  })
})
