import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'
import AiSettingsDialog, {
  resolveSavedAppSettings,
  shouldApplyIncomingAppSettings
} from './AiSettingsDialog.js'

describe('AiSettingsDialog', () => {
  it('renders renamed settings title and screenshot controls', () => {
    const markup = renderToStaticMarkup(
      <AiSettingsDialog
        projectId="project-1"
        initial={{ ai_cli: 'claude' }}
        initialRepoView={{ ai_cli: 'codex' }}
        initialAppSettings={{
          screenshotShortcutEnabled: true,
          screenshotShortcut: 'Alt+Shift+S'
        }}
        onClose={vi.fn()}
        onSaved={vi.fn()}
        onSavedRepoView={vi.fn()}
        onSavedAppSettings={vi.fn()}
      />
    )

    expect(markup).toContain('设置')
    expect(markup).toContain('截图快捷键（全局）')
    expect(markup).toContain('恢复默认')
    expect(markup).toContain('Alt+Shift+S')
  })

  it('renders the no-project AI CLI hint when project is unavailable', () => {
    const markup = renderToStaticMarkup(
      <AiSettingsDialog
        projectId={null}
        initial={{ ai_cli: 'claude' }}
        initialRepoView={{ ai_cli: 'codex' }}
        initialAppSettings={{
          screenshotShortcutEnabled: false,
          screenshotShortcut: 'CommandOrControl+Shift+A'
        }}
        onClose={vi.fn()}
        onSaved={vi.fn()}
        onSavedRepoView={vi.fn()}
        onSavedAppSettings={vi.fn()}
      />
    )

    expect(markup).toContain('选择项目后可编辑 AI CLI 配置')
  })

  it('prefers authoritative saved app settings returned from the backend', () => {
    expect(
      resolveSavedAppSettings(
        {
          screenshotShortcutEnabled: true,
          screenshotShortcut: 'CommandOrControl+Shift+A'
        },
        {
          screenshotShortcutEnabled: false,
          screenshotShortcut: 'Alt+Shift+S'
        }
      )
    ).toEqual({
      screenshotShortcutEnabled: false,
      screenshotShortcut: 'Alt+Shift+S'
    })
  })

  it('preserves local edits when the incoming app-settings prop has not changed', () => {
    expect(
      shouldApplyIncomingAppSettings(
        {
          screenshotShortcutEnabled: true,
          screenshotShortcut: 'CommandOrControl+Shift+A'
        },
        {
          screenshotShortcutEnabled: true,
          screenshotShortcut: 'CommandOrControl+Shift+A'
        },
        false
      )
    ).toBe(false)
  })

  it('does not apply incoming app settings during an active save', () => {
    expect(
      shouldApplyIncomingAppSettings(
        {
          screenshotShortcutEnabled: true,
          screenshotShortcut: 'CommandOrControl+Shift+A'
        },
        {
          screenshotShortcutEnabled: false,
          screenshotShortcut: 'Alt+Shift+S'
        },
        true
      )
    ).toBe(false)
  })
})
