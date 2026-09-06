// 纯模块：不碰 fs/crypto，因此可以进 tsconfig.web 的 include，被渲染端直接引用。
// 需要落盘的读写在 clawCredentials.ts（与 opencodeConfig / opencodeCredentials 同一分法）。

// claw 按**模型名前缀**路由 provider（crates/api/src/providers/openai_compat.rs）：
// `openai/<name>` 走 OpenAI 兼容那条，凭据读 OPENAI_API_KEY、端点读 OPENAI_BASE_URL，
// 且 base URL 不以 /chat/completions 结尾时会自动补上。
// 智谱的 OpenAI 兼容端点正好是这个形状，所以默认值指向它。
//
// 以上不是从文档推的：用本地 mock 端点实跑过一次 `claw --model openai/glm-4.6`，
// 观察到它发出的是
//     POST /chat/completions
//     Authorization: Bearer <OPENAI_API_KEY>
//     {"model":"glm-4.6","stream":true,...}
// 即前缀会被剥掉、Key 以 bearer 形式发送，与智谱的要求一致。
export const CLAW_DEFAULT_BASE_URL = 'https://open.bigmodel.cn/api/paas/v4'
export const CLAW_DEFAULT_MODEL = 'openai/glm-4.6'

export const CLAW_API_KEY_ENV = 'OPENAI_API_KEY'
export const CLAW_BASE_URL_ENV = 'OPENAI_BASE_URL'

// claw 默认把会话写在 <cwd>/.claw/sessions/ —— 而 cwd 是**用户自己的仓库**，
// 等于我们往人家仓库里拉目录。fork 里给 SessionStore::from_cwd 加了这个 env：
// 设了就改用 <CLAW_DATA_DIR>/sessions/<workspace 指纹>/，不同仓库仍互相隔离。
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
  if (!next[CLAW_API_KEY_ENV]) next[CLAW_API_KEY_ENV] = config.apiKey
  if (!next[CLAW_BASE_URL_ENV]) next[CLAW_BASE_URL_ENV] = config.baseUrl || CLAW_DEFAULT_BASE_URL
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
  return ['--model', config.model || CLAW_DEFAULT_MODEL, ...args]
}
