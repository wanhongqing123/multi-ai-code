import { renderToStaticMarkup } from 'react-dom/server'
import type { ComponentProps } from 'react'
import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { describe, expect, it, vi } from 'vitest'
import AiSettingsDialog, {
  DEFAULT_AI_CLI,
  getProjectSettingsRepairToastMessage,
  saveProjectScopedSettings,
  saveRemoteDesktopMode
} from '../../../src/components/AiSettingsDialog.js'
import type { RemoteImConfig } from '../../../electron/remote-im/types'

const LOADED_IM_CONFIG = {
  enabled: true,
  friendUserIds: ['whq-iphone'],
  allowedUserIds: ['whq-iphone'],
  remoteDesktopMode: 'disabled'
} as unknown as RemoteImConfig

function renderDialog(overrides: Partial<ComponentProps<typeof AiSettingsDialog>> = {}) {
  return renderToStaticMarkup(
    <AiSettingsDialog
      projectId="project-1"
      initial={{ ai_cli: 'claude' }}
      onClose={vi.fn()}
      onSaved={vi.fn()}
      {...overrides}
    />
  )
}

describe('AiSettingsDialog', () => {
  it('renders the settings shell with only the AI CLI section', () => {
    const markup = renderDialog()

    expect(markup).toContain('ai-settings-shell')
    expect(markup).toContain('ai-settings-content')
    expect(markup).toContain('ai-settings-footer')
    expect(markup).toContain('id="ai-settings-ai-section"')
    expect(markup).toContain('ai-settings-ai-card')
  })

  // 标题副行、「项目级保存」徽章、卡片头、Binary override、页脚那句提示——
  // 都是没信息量的装饰，删了就别再回来。
  it('drops the decorative header, badge, card head and binary override', () => {
    const markup = renderDialog()

    expect(markup).toContain('<h3>设置</h3>')
    expect(markup).not.toContain('设置中心')
    expect(markup).not.toContain('ai-settings-header-dot')
    expect(markup).not.toContain('ai-settings-project-badge')
    expect(markup).not.toContain('ai-settings-card-head')
    expect(markup).not.toContain('主会话 AI')
    expect(markup).not.toContain('Binary override')
    expect(markup).not.toContain('变更点击')
  })

  it('offers exactly two permission levels and defaults to full access', () => {
    const markup = renderDialog()

    expect(markup).toContain('权限')
    expect(markup).toContain('完全访问权限')
    expect(markup).toContain('默认权限')
    // 没有第三档：不暴露 --dangerously-* 之类的东西给用户挑。
    expect(markup).not.toContain('dangerously')
    expect(markup).toContain('<option value="full-access" selected="">完全访问权限</option>')
  })

  it('honours a saved default-permission choice', () => {
    const markup = renderDialog({ initial: { ai_cli: 'codex', permission_mode: 'default' } })

    expect(markup).toContain('<option value="default" selected="">默认权限</option>')
  })

  // 启动参数不该是常规操作：默认折叠，别摆在主界面上引导用户去填。
  it('keeps launch args behind a collapsed advanced toggle', () => {
    const markup = renderDialog()

    expect(markup).toContain('高级参数设置')
    expect(markup).toContain('aria-expanded="false"')
    expect(markup).not.toContain('附加 args')
    expect(markup).not.toContain('环境变量')
  })

  // 但已经填过参数的项目不能把它藏起来，否则会以为配置丢了。
  it('expands the advanced block when the project already has args or env', () => {
    expect(
      renderDialog({ initial: { ai_cli: 'codex', args: ['--verbose'] } })
    ).toContain('附加 args')
    expect(
      renderDialog({ initial: { ai_cli: 'codex', env: { FOO: 'bar' } } })
    ).toContain('环境变量')
  })

  // 设置界面只保留 AI CLI 一项，全局快捷键 / 工具栏按钮 / 项目构建 / 项目运行
  // 连同背后的功能一起删除了。这条守住「别又长回来」。
  it('keeps every removed settings section out of the dialog', () => {
    const markup = renderDialog()

    expect(markup).not.toContain('ai-settings-sidebar')
    expect(markup).not.toContain('全局快捷键')
    expect(markup).not.toContain('工具栏按钮')
    expect(markup).not.toContain('项目构建')
    expect(markup).not.toContain('项目运行')
    expect(markup).not.toContain('ai-settings-hero-card')
    expect(markup).not.toContain('ai-settings-build-panel')
    expect(markup).not.toContain('ai-settings-runtime-panel')
    expect(markup).not.toContain('id="ai-settings-shortcut-section"')
    expect(markup).not.toContain('id="ai-settings-build-section"')
    expect(markup).not.toContain('id="ai-settings-runtime-section"')
  })

  it('puts Codex first and uses it as the default AI CLI', () => {
    const markup = renderDialog({
      initial: {} as ComponentProps<typeof AiSettingsDialog>['initial']
    })
    const codexOptionIndex = markup.indexOf('Codex (推荐)')
    const claudeOptionIndex = markup.indexOf('Claude Code (不建议使用)')

    expect(DEFAULT_AI_CLI).toBe('codex')
    expect(codexOptionIndex).toBeGreaterThan(-1)
    expect(claudeOptionIndex).toBeGreaterThan(-1)
    expect(codexOptionIndex).toBeLessThan(claudeOptionIndex)
  })

  it('does not expose project-level OpenCode provider settings', () => {
    const markup = renderDialog({
      initial: {
        ai_cli: 'opencode',
        opencode: {
          providerId: 'multi-ai-deepseek-internal',
          name: '公司内网 DeepSeek',
          baseURL: 'https://llm.example.test/v1',
          apiKey: 'test-api-key',
          mainModel: 'deepseek-v4-pro',
          smallModel: 'deepseek-v4-lite'
        }
      }
    })

    expect(markup).not.toContain('OpenCode 模型服务')
    expect(markup).not.toContain('Provider ID')
    expect(markup).not.toContain('multi-ai-deepseek-internal')
    expect(markup).not.toContain('API Key')
  })

  it('keeps remote IM configuration out of the settings center', () => {
    const markup = renderDialog()

    expect(markup).not.toContain('id="ai-settings-remote-im-section"')
    expect(markup).not.toContain('手机消息接入 AICLI')
    expect(markup).not.toContain('AI 输出回传间隔')
    expect(markup).not.toContain('启用远程 IM')
    expect(markup).not.toContain('SECRETKEY')
  })

  it('keeps settings modal sizing stronger than the global modal rule', () => {
    const css = readFileSync(fileURLToPath(new URL('../../../src/styles.css', import.meta.url)), 'utf8')
    const globalModalIndex = css.lastIndexOf('\n.modal {')
    const settingsModalIndex = css.lastIndexOf('.modal.ai-settings-modal')
    const settingsModalRule = css.slice(settingsModalIndex, settingsModalIndex + 360)

    expect(globalModalIndex).toBeGreaterThan(-1)
    expect(settingsModalIndex).toBeGreaterThan(globalModalIndex)
    expect(settingsModalRule).toContain('width: min(820px')
    expect(settingsModalRule).toContain('max-width: calc(100vw - 48px)')
    // 只剩一张卡了，固定高度会留一大片空白：按内容长，上限才是视口。
    expect(settingsModalRule).toContain('height: auto')
    expect(settingsModalRule).toContain('max-height: calc(100vh - 56px)')
  })

  // 侧边导航删了，外层网格必须回到单列，否则左边空出 252px 一条白。
  it('collapses the settings shell to a single column', () => {
    const css = readFileSync(fileURLToPath(new URL('../../../src/styles.css', import.meta.url)), 'utf8')
    const shellIndex = css.indexOf('\n.ai-settings-shell {')
    const shellRule = css.slice(shellIndex, css.indexOf('}', shellIndex))

    expect(shellIndex).toBeGreaterThan(-1)
    expect(shellRule).toContain('grid-template-columns: minmax(0, 1fr)')
    // 断言声明本身，不是整个块——块里的注释也会提到 252px。
    expect(shellRule).not.toContain('grid-template-columns: 252px')
  })

  it('uses compact form text inside the larger settings modal', () => {
    const css = readFileSync(fileURLToPath(new URL('../../../src/styles.css', import.meta.url)), 'utf8')
    const inputRuleIndex = css.indexOf(".ai-settings-card label > input:not([type='checkbox'])")
    const inputRule = css.slice(inputRuleIndex, inputRuleIndex + 620)

    expect(inputRuleIndex).toBeGreaterThan(-1)
    expect(inputRule).toContain('font-size: var(--mac-text-xs)')
  })

  it('renders the no-project AI CLI hint when project is unavailable', () => {
    const markup = renderDialog({ projectId: null })

    expect(markup).toContain('ai-settings-no-project-card')
    expect(markup).toContain('选择工作空间后可编辑 AI CLI 配置')
  })

  it('emits a repair toast when the project settings save repaired metadata', () => {
    expect(getProjectSettingsRepairToastMessage({ ok: true, repaired: false })).toBeNull()
    expect(getProjectSettingsRepairToastMessage({ ok: true, repaired: true })).toBe(
      '项目设置文件已自动修复并保存'
    )
  })

  it('saves the main AI settings and returns the repair toast', async () => {
    const onMainSaved = vi.fn()

    await expect(
      saveProjectScopedSettings({
        projectId: 'project-1',
        nextMain: { ai_cli: 'claude', command: 'claude' },
        setAiSettings: vi.fn().mockResolvedValue({ ok: true, repaired: true }),
        onMainSaved
      })
    ).resolves.toBe('项目设置文件已自动修复并保存')

    expect(onMainSaved).toHaveBeenCalledWith({ ai_cli: 'claude', command: 'claude' })
  })

  it('throws without reporting a save when the main settings write fails', async () => {
    const onMainSaved = vi.fn()

    await expect(
      saveProjectScopedSettings({
        projectId: 'project-1',
        nextMain: { ai_cli: 'claude' },
        setAiSettings: vi.fn().mockResolvedValue({ ok: false, error: 'disk full' }),
        onMainSaved
      })
    ).rejects.toThrow('disk full')

    expect(onMainSaved).not.toHaveBeenCalled()
  })

  it('writes the remote desktop mode back into the untouched IM config', async () => {
    const setConfig = vi.fn().mockResolvedValue({ ok: true })

    await expect(
      saveRemoteDesktopMode({
        projectId: 'project-1',
        loaded: LOADED_IM_CONFIG,
        mode: 'unattended',
        setConfig
      })
    ).resolves.toBe(true)

    // 整份配置回写，只改这一个字段——别处（好友列表等）不能被这次保存抹掉。
    expect(setConfig).toHaveBeenCalledWith('project-1', {
      ...LOADED_IM_CONFIG,
      remoteDesktopMode: 'unattended'
    })
  })

  it('skips the IM config write when the mode did not change', async () => {
    const setConfig = vi.fn()

    await expect(
      saveRemoteDesktopMode({
        projectId: 'project-1',
        loaded: LOADED_IM_CONFIG,
        mode: 'disabled',
        setConfig
      })
    ).resolves.toBe(false)

    expect(setConfig).not.toHaveBeenCalled()
  })

  it('skips the IM config write when the project has no IM config at all', async () => {
    const setConfig = vi.fn()

    await expect(
      saveRemoteDesktopMode({ projectId: 'project-1', loaded: null, mode: 'unattended', setConfig })
    ).resolves.toBe(false)

    expect(setConfig).not.toHaveBeenCalled()
  })

  it('surfaces a failed IM config write instead of closing silently', async () => {
    await expect(
      saveRemoteDesktopMode({
        projectId: 'project-1',
        loaded: LOADED_IM_CONFIG,
        mode: 'unattended',
        setConfig: vi.fn().mockResolvedValue({ ok: false, error: 'config locked' })
      })
    ).rejects.toThrow('config locked')
  })
})
