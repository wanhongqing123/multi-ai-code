import type { OpenCodeProviderProfile } from '../aicli/opencodeConfig.js'

// 渲染层「应用设置」形状：截图快捷键 + 通用界面偏好。由 main 组合后经 IPC 提供。
export interface AppSettings {
  screenshotShortcutEnabled: boolean
  screenshotShortcut: string
  showDevToolbarButtons: boolean
}

// 单个 AI CLI 的配置（主会话 / repo 视图共用）。
export interface AiSettings {
  ai_cli: 'claude' | 'codex' | 'opencode'
  command?: string
  args?: string[]
  env?: Record<string, string>
  opencode?: OpenCodeProviderProfile
}
