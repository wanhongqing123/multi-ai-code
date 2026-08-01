import type { OpenCodeProviderProfile } from '../aicli/opencodeConfig.js'

// 单个 AI CLI 的配置（主会话 / repo 视图共用）。
export interface AiSettings {
  ai_cli: 'claude' | 'codex' | 'opencode'
  command?: string
  args?: string[]
  env?: Record<string, string>
  opencode?: OpenCodeProviderProfile
}
