import { renderToStaticMarkup } from 'react-dom/server'
import type { ComponentProps } from 'react'
import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { describe, expect, it, vi } from 'vitest'
import type {
  ProjectRuntimeConfig,
  VisualStudioInstallation
} from '../../electron/preload'
import AiSettingsDialog, {
  DEFAULT_AI_CLI,
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

const defaultRuntimeConfig: ProjectRuntimeConfig = {
  enabled: false,
  cwd: '.',
  command: '',
  envType: 'msys',
  visualStudioInstanceId: '',
  outputEncoding: 'auto'
}

const defaultDialogProps = {
  visualStudioInstallations: [] as VisualStudioInstallation[],
  visualStudioInstallationsLoading: false,
  onRefreshVisualStudioInstallations: vi.fn(),
  mainCliLabel: 'Claude Code',
  initialRuntimeConfig: defaultRuntimeConfig,
  runtimeConfigReady: true,
  runtimeConfigDisabled: false,
  onSavedRuntimeConfig: vi.fn()
}

function renderDialog(overrides: Partial<ComponentProps<typeof AiSettingsDialog>> = {}) {
  return renderToStaticMarkup(
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
      {...overrides}
    />
  )
}

describe('AiSettingsDialog', () => {
  it('renders the redesigned settings center shell and screenshot controls', () => {
    const markup = renderDialog()

    expect(markup).toContain('ai-settings-shell')
    expect(markup).toContain('ai-settings-sidebar')
    expect(markup).toContain('ai-settings-content')
    expect(markup).toContain('ai-settings-footer')
    expect(markup).toContain('ai-settings-hero-card')
    expect(markup).toContain('Alt+Shift+S')
  })

  it('puts Codex first and uses it as the default AI CLI', () => {
    const markup = renderDialog({
      initial: {} as ComponentProps<typeof AiSettingsDialog>['initial'],
      initialRepoView: {} as ComponentProps<typeof AiSettingsDialog>['initialRepoView']
    })
    const codexOptionIndex = markup.indexOf('Codex (推荐)')
    const claudeOptionIndex = markup.indexOf('Claude Code (不建议使用)')

    expect(DEFAULT_AI_CLI).toBe('codex')
    expect(codexOptionIndex).toBeGreaterThan(-1)
    expect(claudeOptionIndex).toBeGreaterThan(-1)
    expect(codexOptionIndex).toBeLessThan(claudeOptionIndex)
  })

  it('renders OpenCode provider profile fields when OpenCode is selected', () => {
    const markup = renderDialog({
      initial: {
        ai_cli: 'opencode',
        opencode: {
          providerId: 'multi-ai-deepseek-internal',
          name: '公司内网 DeepSeek',
          baseURL: 'https://llm.example.test/v1',
          apiKeyEnvVar: 'DEEPSEEK_INTERNAL_API_KEY',
          mainModel: 'deepseek-v4-pro',
          smallModel: 'deepseek-v4-lite'
        }
      }
    })

    expect(markup).toContain('OpenCode 模型服务')
    expect(markup).toContain('Provider ID')
    expect(markup).toContain('multi-ai-deepseek-internal')
    expect(markup).toContain('https://llm.example.test/v1')
    expect(markup).toContain('DEEPSEEK_INTERNAL_API_KEY')
    expect(markup).toContain('deepseek-v4-pro')
    expect(markup).toContain('deepseek-v4-lite')
  })

  it('renders sidebar navigation as clickable section buttons', () => {
    const markup = renderDialog()

    expect(markup).toContain('type="button" class="ai-settings-nav-item active"')
    expect(markup).toContain('aria-controls="ai-settings-shortcut-section"')
    expect(markup).toContain('aria-controls="ai-settings-ai-section"')
    expect(markup).toContain('aria-controls="ai-settings-build-section"')
    expect(markup).toContain('aria-controls="ai-settings-runtime-section"')
    expect(markup).not.toContain('aria-controls="ai-settings-remote-im-section"')
    expect(markup).toContain('id="ai-settings-shortcut-section"')
    expect(markup).toContain('id="ai-settings-ai-section"')
  })

  it('keeps remote IM configuration out of the settings center', () => {
    const markup = renderDialog()

    expect(markup).not.toContain('id="ai-settings-remote-im-section"')
    expect(markup).not.toContain('手机消息接入 AICLI')
    expect(markup).not.toContain('AI 输出回传间隔')
    expect(markup).not.toContain('启用远程 IM')
    expect(markup).not.toContain('SECRETKEY')
  })

  it('renders habit monitor as an embedded settings section', () => {
    const markup = renderDialog()

    expect(markup).toContain('ai-settings-habit-entry')
    expect(markup).toContain('aria-controls="ai-settings-habit-section"')
    expect(markup).toContain('id="ai-settings-habit-section"')
    expect(markup).toContain('habit-monitor-panel')
    expect(markup).toContain('习惯监控')
    expect(markup).toContain('查看活跃流程和采集设置')
  })

  it('keeps settings modal sizing stronger than the global modal rule', () => {
    const css = readFileSync(fileURLToPath(new URL('../styles.css', import.meta.url)), 'utf8')
    const globalModalIndex = css.lastIndexOf('\n.modal {')
    const settingsModalIndex = css.lastIndexOf('.modal.ai-settings-modal')
    const settingsModalRule = css.slice(settingsModalIndex, settingsModalIndex + 360)

    expect(globalModalIndex).toBeGreaterThan(-1)
    expect(settingsModalIndex).toBeGreaterThan(globalModalIndex)
    expect(settingsModalRule).toContain('width: min(1180px')
    expect(settingsModalRule).toContain('max-width: calc(100vw - 48px)')
    expect(settingsModalRule).toContain('height: min(860px')
  })

  it('uses compact form text inside the larger settings modal', () => {
    const css = readFileSync(fileURLToPath(new URL('../styles.css', import.meta.url)), 'utf8')
    const inputRuleIndex = css.indexOf(".ai-settings-card label > input:not([type='checkbox'])")
    const inputRule = css.slice(inputRuleIndex, inputRuleIndex + 620)

    expect(inputRuleIndex).toBeGreaterThan(-1)
    expect(inputRule).toContain('font-size: var(--mac-text-xs)')
  })

  it('renders the project runtime settings section', () => {
    const markup = renderDialog()

    expect(markup).toContain('ai-settings-runtime-panel')
    expect(markup).toContain('project-runtime-settings-grid')
  })

  it('renders preset shortcut buttons and hides the custom input until custom mode is needed', () => {
    const markup = renderDialog({
      initialAppSettings: {
        screenshotShortcutEnabled: true,
        screenshotShortcut: 'CommandOrControl+Shift+S'
      }
    })

    expect(markup).toContain('Ctrl/Cmd + Shift + A')
    expect(markup).toContain('Ctrl/Cmd + Shift + S')
    expect(markup).toContain('Ctrl/Cmd + Alt + A')
    expect(markup).toContain('Alt + Shift + A')
    expect(markup).toContain('ai-settings-shortcut-custom-toggle')
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
    const markup = renderDialog({
      initialAppSettings: {
        screenshotShortcutEnabled: true,
        screenshotShortcut: 'Shift+Meta+K'
      }
    })

    expect(markup).toContain('Ctrl/Cmd + Shift + A')
    expect(markup).toContain('ai-settings-shortcut-custom-toggle')
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
    const markup = renderDialog({
      projectId: null,
      initialAppSettings: {
        screenshotShortcutEnabled: false,
        screenshotShortcut: 'CommandOrControl+Shift+A'
      },
      buildConfigReady: false
    })

    expect(markup).toContain('ai-settings-no-project-card')
    expect(markup).toContain('ai-settings-build-panel')
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

  it('saves runtime config after the other project settings succeed', async () => {
    const onRuntimeConfigSaved = vi.fn()
    const setRuntimeConfig = vi.fn().mockResolvedValue({ ok: true })

    await expect(
      saveProjectScopedSettings({
        projectId: 'project-1',
        nextMain: { ai_cli: 'claude' },
        nextRepoView: { ai_cli: 'codex' },
        nextBuildConfig: defaultBuildConfig,
        nextRuntimeConfig: defaultRuntimeConfig,
        setAiSettings: vi.fn().mockResolvedValue({ ok: true, repaired: false }),
        setRepoViewAiSettings: vi.fn().mockResolvedValue({ ok: true, repaired: false }),
        setBuildConfig: vi.fn().mockResolvedValue({ ok: true, repaired: false }),
        setRuntimeConfig,
        onMainSaved: vi.fn(),
        onRepoViewSaved: vi.fn(),
        onBuildConfigSaved: vi.fn(),
        onRuntimeConfigSaved
      })
    ).resolves.toBeNull()

    expect(setRuntimeConfig).toHaveBeenCalledWith('project-1', defaultRuntimeConfig)
    expect(onRuntimeConfigSaved).toHaveBeenCalledWith(defaultRuntimeConfig)
  })

  it('reports runtime config validation failures without calling the runtime saved callback', async () => {
    const onRuntimeConfigSaved = vi.fn()

    await expect(
      saveProjectScopedSettings({
        projectId: 'project-1',
        nextMain: { ai_cli: 'claude' },
        nextRepoView: { ai_cli: 'codex' },
        nextRuntimeConfig: { ...defaultRuntimeConfig, enabled: true, command: '' },
        setAiSettings: vi.fn().mockResolvedValue({ ok: true, repaired: false }),
        setRepoViewAiSettings: vi.fn().mockResolvedValue({ ok: true, repaired: false }),
        setRuntimeConfig: vi.fn().mockResolvedValue({
          ok: false,
          error: 'invalid runtime config',
          details: [{ path: 'runtime_config.command', message: 'command must be a non-empty string' }]
        }),
        onMainSaved: vi.fn(),
        onRepoViewSaved: vi.fn(),
        onRuntimeConfigSaved
      })
    ).rejects.toThrow('invalid runtime config')

    expect(onRuntimeConfigSaved).not.toHaveBeenCalled()
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
