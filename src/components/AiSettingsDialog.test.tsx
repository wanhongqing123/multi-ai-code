import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'
import type { VisualStudioInstallation } from '../../electron/preload'
import AiSettingsDialog, {
  deriveAppSettingsSaveOutcome,
  getProjectSettingsRepairToastMessage,
  resolveSavedAppSettings,
  saveProjectScopedSettings,
  shouldApplyIncomingAppSettings
} from './AiSettingsDialog.js'

const defaultBuildConfig = {
  enabled: false,
  steps: []
}

const defaultDialogProps = {
  visualStudioInstallations: [] as VisualStudioInstallation[],
  visualStudioInstallationsLoading: false,
  onRefreshVisualStudioInstallations: vi.fn()
}

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
        initialBuildConfig={defaultBuildConfig}
        buildConfigReady={true}
        {...defaultDialogProps}
        onClose={vi.fn()}
        onSaved={vi.fn()}
        onSavedRepoView={vi.fn()}
        onSavedAppSettings={vi.fn()}
        onSavedBuildConfig={vi.fn()}
      />
    )

    expect(markup).toContain('设置')
    expect(markup).toContain('截图快捷键（全局）')
    expect(markup).toContain('恢复默认')
    expect(markup).toContain('Alt+Shift+S')
  })

  it('renders preset shortcut buttons and hides the custom input until custom mode is needed', () => {
    const markup = renderToStaticMarkup(
      <AiSettingsDialog
        projectId="project-1"
        initial={{ ai_cli: 'claude' }}
        initialRepoView={{ ai_cli: 'codex' }}
        initialAppSettings={{
          screenshotShortcutEnabled: true,
          screenshotShortcut: 'CommandOrControl+Shift+S'
        }}
        initialBuildConfig={defaultBuildConfig}
        buildConfigReady={true}
        {...defaultDialogProps}
        onClose={vi.fn()}
        onSaved={vi.fn()}
        onSavedRepoView={vi.fn()}
        onSavedAppSettings={vi.fn()}
        onSavedBuildConfig={vi.fn()}
      />
    )

    expect(markup).toContain('Ctrl/Cmd + Shift + A')
    expect(markup).toContain('Ctrl/Cmd + Shift + S')
    expect(markup).toContain('Ctrl/Cmd + Alt + A')
    expect(markup).toContain('Alt + Shift + A')
    expect(markup).toContain('自定义')
    expect(markup).not.toContain('placeholder="CommandOrControl+Shift+A"')
  })

  it('opens the custom editor when the current shortcut is not a preset', async () => {
    const module = await import('./AiSettingsDialog.js')

    expect(module.isPresetScreenshotShortcut('CommandOrControl+Shift+A')).toBe(true)
    expect(module.isPresetScreenshotShortcut('Shift+Meta+K')).toBe(false)
    expect(module.createScreenshotShortcutState('Shift+Meta+K')).toEqual({
      screenshotShortcut: 'Shift+Meta+K',
      customExpanded: true
    })
  })

  it('keeps preset buttons visible when custom mode is expanded for a non-preset shortcut', () => {
    const markup = renderToStaticMarkup(
      <AiSettingsDialog
        projectId="project-1"
        initial={{ ai_cli: 'claude' }}
        initialRepoView={{ ai_cli: 'codex' }}
        initialAppSettings={{
          screenshotShortcutEnabled: true,
          screenshotShortcut: 'Shift+Meta+K'
        }}
        initialBuildConfig={defaultBuildConfig}
        buildConfigReady={true}
        {...defaultDialogProps}
        onClose={vi.fn()}
        onSaved={vi.fn()}
        onSavedRepoView={vi.fn()}
        onSavedAppSettings={vi.fn()}
        onSavedBuildConfig={vi.fn()}
      />
    )

    expect(markup).toContain('Ctrl/Cmd + Shift + A')
    expect(markup).toContain('自定义')
    expect(markup).toContain('placeholder="CommandOrControl+Shift+A"')
  })

  it('collapses the custom editor when a preset is chosen or defaults are restored', async () => {
    const module = await import('./AiSettingsDialog.js')

    expect(module.selectScreenshotShortcutPreset('CommandOrControl+Shift+S')).toEqual({
      screenshotShortcut: 'CommandOrControl+Shift+S',
      customExpanded: false
    })
    expect(module.restoreDefaultScreenshotShortcut()).toEqual({
      screenshotShortcut: 'CommandOrControl+Shift+A',
      customExpanded: false
    })
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
        initialBuildConfig={defaultBuildConfig}
        buildConfigReady={false}
        {...defaultDialogProps}
        onClose={vi.fn()}
        onSaved={vi.fn()}
        onSavedRepoView={vi.fn()}
        onSavedAppSettings={vi.fn()}
        onSavedBuildConfig={vi.fn()}
      />
    )

    expect(markup).toContain('选择项目后可编辑 AI CLI 配置')
    expect(markup).toContain('选择项目后可编辑项目构建配置')
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

  it('uses authoritative fallback app settings from a failed save response', () => {
    expect(
      deriveAppSettingsSaveOutcome(
        {
          screenshotShortcutEnabled: true,
          screenshotShortcut: 'Attempted+Shortcut'
        },
        {
          ok: false,
          value: {
            screenshotShortcutEnabled: false,
            screenshotShortcut: 'Fallback+Shortcut'
          },
          error: 'save failed'
        }
      )
    ).toEqual({
      appSettings: {
        screenshotShortcutEnabled: false,
        screenshotShortcut: 'Fallback+Shortcut'
      },
      error: 'save failed'
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

  it('emits a repair toast when any project settings save repaired metadata', () => {
    expect(
      getProjectSettingsRepairToastMessage(
        { ok: true, repaired: false },
        { ok: true, repaired: false }
      )
    ).toBeNull()
    expect(
      getProjectSettingsRepairToastMessage(
        { ok: true, repaired: false },
        { ok: true, repaired: false },
        { ok: true, repaired: true }
      )
    ).toBe('项目设置文件已自动修复并保存')
  })
  it('syncs main and repo AI settings even when build-config save fails', async () => {
    const onMainSaved = vi.fn()
    const onRepoViewSaved = vi.fn()
    const onBuildConfigSaved = vi.fn()

    await expect(
      saveProjectScopedSettings({
        projectId: 'project-1',
        nextMain: { ai_cli: 'claude', command: 'claude' },
        nextRepoView: { ai_cli: 'codex', command: 'codex' },
        nextBuildConfig: defaultBuildConfig,
        setAiSettings: vi.fn().mockResolvedValue({ ok: true }),
        setRepoViewAiSettings: vi.fn().mockResolvedValue({ ok: true }),
        setBuildConfig: vi.fn().mockResolvedValue({
          ok: false,
          error: 'invalid build config',
          details: [{ path: 'build_config.steps[0].cwd', message: 'cwd invalid' }]
        }),
        onMainSaved,
        onRepoViewSaved,
        onBuildConfigSaved
      })
    ).rejects.toThrow('invalid build config')

    expect(onMainSaved).toHaveBeenCalledWith({ ai_cli: 'claude', command: 'claude' })
    expect(onRepoViewSaved).toHaveBeenCalledWith({ ai_cli: 'codex', command: 'codex' })
    expect(onBuildConfigSaved).not.toHaveBeenCalled()
  })

  it('returns a repair toast after all project saves succeed', async () => {
    const onMainSaved = vi.fn()
    const onRepoViewSaved = vi.fn()
    const onBuildConfigSaved = vi.fn()

    const expectedToast = getProjectSettingsRepairToastMessage(
      { ok: true, repaired: false },
      { ok: true, repaired: false },
      { ok: true, repaired: true }
    )

    await expect(
      saveProjectScopedSettings({
        projectId: 'project-1',
        nextMain: { ai_cli: 'claude' },
        nextRepoView: { ai_cli: 'codex' },
        nextBuildConfig: defaultBuildConfig,
        setAiSettings: vi.fn().mockResolvedValue({ ok: true, repaired: false }),
        setRepoViewAiSettings: vi.fn().mockResolvedValue({ ok: true, repaired: false }),
        setBuildConfig: vi.fn().mockResolvedValue({ ok: true, repaired: true }),
        onMainSaved,
        onRepoViewSaved,
        onBuildConfigSaved
      })
    ).resolves.toBe(expectedToast)

    expect(onBuildConfigSaved).toHaveBeenCalledWith(defaultBuildConfig)
  })

  it('skips build-config persistence when the current project config is still loading', async () => {
    const setBuildConfig = vi.fn()

    await expect(
      saveProjectScopedSettings({
        projectId: 'project-1',
        nextMain: { ai_cli: 'claude' },
        nextRepoView: { ai_cli: 'codex' },
        setAiSettings: vi.fn().mockResolvedValue({ ok: true, repaired: false }),
        setRepoViewAiSettings: vi.fn().mockResolvedValue({ ok: true, repaired: false }),
        setBuildConfig,
        onMainSaved: vi.fn(),
        onRepoViewSaved: vi.fn()
      })
    ).resolves.toBeNull()

    expect(setBuildConfig).not.toHaveBeenCalled()
  })
})
