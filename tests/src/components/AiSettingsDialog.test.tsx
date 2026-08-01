import { renderToStaticMarkup } from 'react-dom/server'
import type { ComponentProps } from 'react'
import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { describe, expect, it, vi } from 'vitest'
import AiSettingsDialog, {
  DEFAULT_AI_CLI,
  getProjectSettingsRepairToastMessage,
  saveProjectScopedSettings
} from '../../../src/components/AiSettingsDialog.js'

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

  it('renders OpenCode provider profile fields when OpenCode is selected', () => {
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

    expect(markup).toContain('OpenCode 模型服务')
    expect(markup).toContain('Provider ID')
    expect(markup).toContain('multi-ai-deepseek-internal')
    expect(markup).toContain('https://llm.example.test/v1')
    expect(markup).toContain('API Key')
    expect(markup).toContain('type="password"')
    expect(markup).toContain('deepseek-v4-pro')
    expect(markup).toContain('deepseek-v4-lite')
    expect(markup).not.toContain('Alibaba ideaLAB')
  })

  it('prefills Alibaba ideaLAB defaults for new OpenCode settings without an API key', () => {
    const markup = renderDialog({
      initial: {
        ai_cli: 'opencode'
      }
    })

    expect(markup).toContain('Alibaba ideaLAB')
    expect(markup).toContain('idealab')
    expect(markup).toContain('https://idealab.alibaba-inc.com/api/openai/v1')
    expect(markup).toContain('Qwen3.7-Max-DogFooding')
    expect(markup).toContain('https://aistudio.alibaba-inc.com/#/aistudio/manage/accountManage')
    expect(markup).toContain('<input type="password" placeholder="sk-..." value=""/>')
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
})
