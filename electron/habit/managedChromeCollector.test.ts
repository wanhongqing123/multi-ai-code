import { describe, expect, it } from 'vitest'
import {
  buildSiteClickEvent,
  buildSiteInputHintEvent,
  buildSiteVisitEvent
} from './managedChromeCollector.js'

describe('buildSiteVisitEvent', () => {
  it('maps a page visit to a managed-chrome site_visit event', () => {
    const event = buildSiteVisitEvent('https://example.test/docs/getting-started?from=nav', 'tab-1')

    expect(event).toMatchObject({
      kind: 'site_visit',
      source: 'managed_chrome'
    })
    expect(event.text).toContain('example.test')
    expect(event.extras).toMatchObject({
      actionType: 'navigate',
      tabId: 'tab-1',
      url: 'https://example.test/docs/getting-started?from=nav'
    })
  })
})

describe('buildSiteClickEvent', () => {
  it('maps click metadata to a site_click hint without dropping the target identity', () => {
    const event = buildSiteClickEvent({
      tabId: 'tab-2',
      url: 'https://example.test/dashboard',
      role: 'button',
      label: 'Open Build Logs',
      selectorHint: 'button[data-testid="build-logs"]'
    })

    expect(event).toMatchObject({
      kind: 'site_click',
      source: 'managed_chrome'
    })
    expect(event.text).toContain('Open Build Logs')
    expect(event.extras).toMatchObject({
      actionType: 'click',
      role: 'button',
      elementHint: 'Open Build Logs'
    })
  })
})

describe('buildSiteInputHintEvent', () => {
  it('keeps only non-sensitive input hints and drops the raw typed value', () => {
    const event = buildSiteInputHintEvent({
      tabId: 'tab-3',
      url: 'https://example.test/search',
      type: 'search',
      label: 'Search docs',
      placeholder: 'Search docs',
      value: 'secret keyword'
    })

    expect(event).not.toBeNull()
    expect(event).toMatchObject({
      kind: 'site_input_hint',
      source: 'managed_chrome'
    })
    expect(event?.text).toContain('Search docs')
    expect(event?.text).not.toContain('secret keyword')
    expect(event?.extras).toMatchObject({
      actionType: 'input',
      inputType: 'search',
      elementHint: 'Search docs'
    })
  })

  it('drops password fields entirely', () => {
    const event = buildSiteInputHintEvent({
      tabId: 'tab-4',
      url: 'https://example.test/login',
      type: 'password',
      label: 'Password',
      value: 'super-secret'
    })

    expect(event).toBeNull()
  })
})
