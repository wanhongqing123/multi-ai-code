// 纯模块：不碰 fs/crypto，因此可以进 tsconfig.web 的 include，被渲染端直接引用。
// 需要落盘的读写在 clawCredentials.ts（与 opencodeConfig / opencodeCredentials 同一分法）。

// claw 按**模型名前缀**路由 provider（crates/api/src/providers/mod.rs 的 provider 元数据表）：
//
//   anthropic/<name>            → Anthropic     读 ANTHROPIC_API_KEY / ANTHROPIC_BASE_URL
//   openai/<name>、gpt-*        → OpenAI 兼容   读 OPENAI_API_KEY    / OPENAI_BASE_URL
//   grok* / qwen* / kimi* ...   → 各自的 provider
//   **无前缀 → 不匹配任何一条 → 落到「按已设置的鉴权变量嗅探」**
//
// 最后那条是个坑：只要我们设了 OPENAI_API_KEY，无前缀模型就会被嗅探成 OpenAI。
//
// 智谱同时提供两种兼容端点，实测两条都能用（用真实 Key 打过）：
//   https://open.bigmodel.cn/api/coding/paas/v4   OpenAI 兼容    → 200
//   https://open.bigmodel.cn/api/anthropic        Anthropic 兼容 → 200
// 而**不带 /coding/ 的普通 OpenAI 端点对 Coding Plan 的 Key 返回 429「无可用资源包」**，
// 后端状态不同也会表现成 {"code":500,"msg":"404 NOT_FOUND"}。
//
// 默认值必须与 resources/opencode/managed-models.json 里的智谱条目一致，有用例钉住。
export const CLAW_DEFAULT_BASE_URL = 'https://open.bigmodel.cn/api/coding/paas/v4'
export const CLAW_DEFAULT_MODEL = 'openai/glm-5.3'

/** Anthropic 端点的默认值，供用户把模型换成 anthropic/ 前缀时使用。 */
export const CLAW_ANTHROPIC_BASE_URL = 'https://open.bigmodel.cn/api/anthropic'

export const CLAW_OPENAI_KEY_ENV = 'OPENAI_API_KEY'
export const CLAW_OPENAI_BASE_URL_ENV = 'OPENAI_BASE_URL'
export const CLAW_ANTHROPIC_KEY_ENV = 'ANTHROPIC_API_KEY'
export const CLAW_ANTHROPIC_BASE_URL_ENV = 'ANTHROPIC_BASE_URL'

/**
 * 模型名决定该往哪一组 env 写凭据和端点。
 *
 * **这是一个真实事故的修复**：此前无论模型走哪个 provider，代码都把用户填的 Base URL
 * 写进 OPENAI_BASE_URL。用户把 Base URL 填成 Anthropic 端点、模型填 `glm-5.3`（无前缀），
 * 结果无前缀被嗅探成 OpenAI，claw 就往
 *     https://open.bigmodel.cn/api/anthropic/chat/completions
 * 发请求——这个路径不存在，于是 404 NOT_FOUND。
 * 一个 Base URL 输入框对应两组互不相干的 env，必须按前缀分派。
 */
export function clawProviderForModel(model: string): 'anthropic' | 'openai' {
  return model.trim().toLowerCase().startsWith('anthropic/') ? 'anthropic' : 'openai'
}

export const CLAW_DATA_DIR_ENV = 'CLAW_DATA_DIR'

export interface ClawManagedConfig {
  apiKey: string
  /** 空表示用 CLAW_DEFAULT_BASE_URL。 */
  baseUrl: string
  /** 空表示用 CLAW_DEFAULT_MODEL；必须带 provider 前缀，否则 claw 会按 Anthropic 路由。 */
  model: string
}

export const EMPTY_CLAW_CONFIG: ClawManagedConfig = { apiKey: '', baseUrl: '', model: '' }

export function isClawCommand(command: string): boolean {
  let normalized = command.trim()
  while (normalized.length >= 2) {
    const first = normalized[0]
    const last = normalized[normalized.length - 1]
    if ((first === '"' && last === '"') || (first === "'" && last === "'")) {
      normalized = normalized.slice(1, -1).trim()
      continue
    }
    break
  }
  const base = (normalized.split(/[\\/]+/).pop() ?? normalized).toLowerCase()
  // 必须两端锚定：claw workspace 里还有 claw-analog / claw-rag-service，
  // 少了 $ 它们会被当成 claw 处理。
  return /^claw(\.(exe|cmd|bat|ps1))?$/.test(base)
}

/**
 * 把托管配置合成进 claw 的启动环境。用户在设置里显式写的同名 env 优先——
 * 那是他们指向别的 provider（xAI / DashScope / Ollama / 自建）的唯一出口。
 */
export function withClawManagedEnv(
  command: string,
  env: Record<string, string> | undefined,
  config: ClawManagedConfig
): Record<string, string> | undefined {
  if (!isClawCommand(command)) return env
  const next = { ...(env ?? {}) }
  if (!config.apiKey) return next

  // 必须用规范化后的名字：withClawModelArgs 传给 claw 的是规范化结果，
  // 这里若用原始值判定，填裸名时 env 会指向另一个 provider —— 正是这次事故的形状。
  const provider = clawProviderForModel(clawCanonicalModel(config.model))
  const keyEnv = provider === 'anthropic' ? CLAW_ANTHROPIC_KEY_ENV : CLAW_OPENAI_KEY_ENV
  const urlEnv = provider === 'anthropic' ? CLAW_ANTHROPIC_BASE_URL_ENV : CLAW_OPENAI_BASE_URL_ENV
  const fallbackUrl = provider === 'anthropic' ? CLAW_ANTHROPIC_BASE_URL : CLAW_DEFAULT_BASE_URL

  if (!next[keyEnv]) next[keyEnv] = config.apiKey
  if (!next[urlEnv]) next[urlEnv] = config.baseUrl || fallbackUrl
  return next
}

/**
 * 把会话目录挪出用户仓库。与凭据分开是刻意的：**没配 API Key 也要生效**——
 * 仓库污染和有没有配置 provider 无关。
 */
export function withClawDataDirEnv(
  command: string,
  env: Record<string, string> | undefined,
  dataDir: string
): Record<string, string> | undefined {
  if (!isClawCommand(command)) return env
  const next = { ...(env ?? {}) }
  if (!next[CLAW_DATA_DIR_ENV] && dataDir) next[CLAW_DATA_DIR_ENV] = dataDir
  return next
}

/**
 * 补上 `--model`。claw 缺省走 anthropic/claude-*，在托管（智谱）配置下必然认证失败，
 * 所以没显式指定时要注入一个带 provider 前缀的模型名。
 * 用户自己在 args 里写了 --model 就完全不动——包括他写了别的 provider 的情况。
 */
export function withClawModelArgs(
  command: string,
  args: readonly string[],
  config: ClawManagedConfig
): string[] {
  if (!isClawCommand(command)) return [...args]
  if (args.some((arg) => arg === '--model' || arg.startsWith('--model='))) return [...args]
  if (!config.apiKey) return [...args]
  return ['--model', clawCanonicalModel(config.model), ...args]
}

/**
 * 把设置里填的模型名补成带 provider 前缀的形式。
 *
 * claw 对**无前缀**的模型名不做前缀路由，而是落到「按已设置的鉴权变量嗅探」——
 * 那条路径的结果取决于我们注入了哪些 env，用户完全无法预期。
 * 设置界面已经写明「必须带 provider 前缀」，但只靠提示文字挡不住；
 * 这里按前缀分派的同一套规则把它补齐，让路由变成确定的。
 *
 * 只认已知前缀，其余一律按 OpenAI 兼容补 `openai/`——智谱两种端点里
 * 我们的默认走的就是 OpenAI 兼容那条。
 */
export function clawCanonicalModel(model: string): string {
  const trimmed = model.trim()
  if (!trimmed) return CLAW_DEFAULT_MODEL
  const known = ['anthropic/', 'openai/', 'local/', 'qwen/', 'kimi/', 'xai/']
  if (known.some((prefix) => trimmed.toLowerCase().startsWith(prefix))) return trimmed
  // 这些裸名在 claw 里有自己的路由规则，不要画蛇添足加前缀。
  if (/^(gpt-|grok|qwen-|kimi-)/i.test(trimmed)) return trimmed
  return `openai/${trimmed}`
}
