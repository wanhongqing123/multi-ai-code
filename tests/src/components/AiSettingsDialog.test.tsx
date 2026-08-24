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
  friendUserIds: ['whq-iphone'],
  remoteDesktopMode: 'disabled' as const,
  remoteDesktopControl: false
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

  it('offers three permission levels and defaults to full access', () => {
    const markup = renderDialog()

    expect(markup).toContain('权限')
    expect(markup).toContain('完全访问权限')
    expect(markup).toContain('危险模式')
    expect(markup).toContain('默认权限')
    expect(markup).not.toContain('dangerously')
    expect(markup).toContain('<option value="full-access" selected="">完全访问权限</option>')
  })

  it('shows an explicit destructive-command warning for dangerous mode', () => {
    const markup = renderDialog({
      initial: { ai_cli: 'codex', permission_mode: 'dangerous' }
    })

    expect(markup).toContain('<option value="dangerous" selected="">危险模式</option>')
    expect(markup).toContain('无沙箱、无审批')
    expect(markup).toContain('递归强制删除也会立即执行')
    expect(markup).toContain('role="alert"')
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

  it('shows only the local Zhipu API Key setting for OpenCode', () => {
    const markup = renderDialog({
      initial: {
        ai_cli: 'opencode',
        opencode: {
          providerId: 'legacy-custom-provider',
          name: '旧的项目级模型服务',
          baseURL: 'https://llm.example.test/v1',
          apiKey: 'test-api-key',
          mainModel: 'legacy-model',
          smallModel: 'legacy-small-model'
        }
      }
    })

    expect(markup).not.toContain('OpenCode 模型服务')
    expect(markup).not.toContain('Provider ID')
    expect(markup).not.toContain('legacy-custom-provider')
    expect(markup).toContain('智谱 API Key')
    expect(markup).toContain('type="password"')
    expect(markup).toContain('不会写入项目仓库或安装包')
    expect(markup).not.toContain('test-api-key')
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

  // 从「远程桌面」开始设置里不止一张卡了，卡与卡之间必须留白，
  // 否则第二张的标题会被挤在两张卡的交界线上。
  it('spaces the settings cards apart', () => {
    const css = readFileSync(fileURLToPath(new URL('../../../src/styles.css', import.meta.url)), 'utf8')
    const contentIndex = css.indexOf('\n.ai-settings-content {')
    const contentRule = css.slice(contentIndex, css.indexOf('}', contentIndex))

    expect(contentIndex).toBeGreaterThan(-1)
    expect(contentRule).toContain('flex-direction: column')
    // 间距比卡内的 sp-4 大一档，卡的分界才比卡内各行的分界更明显。
    expect(contentRule).toContain('gap: var(--mac-sp-5)')
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
    const saved = { ...LOADED_IM_CONFIG, remoteDesktopMode: 'unattended' }
    const setConfig = vi.fn().mockResolvedValue({ ok: true, value: saved })

    // 必须回吐主进程存下的那份配置：被控端是从 props.config 实时读开关的，
    // 只写磁盘不回灌，开关要等切项目或重启才生效，界面却显示已保存。
    await expect(
      saveRemoteDesktopMode({
        projectId: 'project-1',
        loaded: LOADED_IM_CONFIG,
        mode: 'unattended',
        control: false,
        setConfig
      })
    ).resolves.toBe(saved)

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
        control: false,
        setConfig
      })
    ).resolves.toBeNull()

    expect(setConfig).not.toHaveBeenCalled()
  })

  it('skips the IM config write when the project has no IM config at all', async () => {
    const setConfig = vi.fn()

    await expect(
      saveRemoteDesktopMode({
        projectId: 'project-1',
        loaded: null,
        mode: 'unattended',
        control: false,
        setConfig
      })
    ).resolves.toBeNull()

    expect(setConfig).not.toHaveBeenCalled()
  })

  it('returns the config the main process stored, not the one it sent', async () => {
    // 主进程会把账号库里的凭证和联系人合并回来，回吐的这份才是完整的。
    // 直接把发出去的那份灌回状态，会把好友列表抹成空。
    const merged = {
      ...LOADED_IM_CONFIG,
      remoteDesktopMode: 'unattended',
      desktopUserId: 'house-multi-ai-code',
      friendUserIds: ['whq-iphone', 'house-iphone']
    }

    await expect(
      saveRemoteDesktopMode({
        projectId: 'project-1',
        loaded: LOADED_IM_CONFIG,
        mode: 'unattended',
        control: false,
        setConfig: vi.fn().mockResolvedValue({ ok: true, value: merged })
      })
    ).resolves.toBe(merged)
  })

  it('surfaces a failed IM config write instead of closing silently', async () => {
    await expect(
      saveRemoteDesktopMode({
        projectId: 'project-1',
        loaded: LOADED_IM_CONFIG,
        mode: 'unattended',
        control: false,
        setConfig: vi.fn().mockResolvedValue({ ok: false, error: 'config locked' })
      })
    ).rejects.toThrow('config locked')
  })
})
