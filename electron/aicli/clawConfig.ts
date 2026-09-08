// 纯模块：不碰 fs/crypto，因此可以进 tsconfig.web 的 include，被渲染端直接引用。
// 需要落盘的读写在 clawCredentials.ts。

/**
 * 设计取向：**协议是显式选项，模型是自由文本，端点可改。**
 *
 * 刻意**不**采用 opencode 那套「宿主维护一份策展目录、用户只能从白名单里挑」的做法：
 * 那是围墙花园，而 claw 本身是开放的——设好 env、写上模型名，它就按前缀路由。
 * 我们要做的只是把「协议 ↔ 该写哪组 env」这层机械对应关系接好，不去替用户决定能用什么模型。
 * （opencode 即将废弃，它的资源文件和设计思路都不该被 claw 继承。）
 *
 * claw 内部按模型名前缀路由（crates/api/src/providers/mod.rs 的元数据表），
 * 每个协议有自己的一组 env：
 *
 *   前缀          鉴权 env              端点 env
 *   anthropic/    ANTHROPIC_API_KEY     ANTHROPIC_BASE_URL
 *   openai/       OPENAI_API_KEY        OPENAI_BASE_URL
 *   xai/、grok*   XAI_API_KEY           XAI_BASE_URL
 *   qwen*、kimi*  DASHSCOPE_API_KEY     DASHSCOPE_BASE_URL
 *   local/        （无）                OLLAMA_HOST
 *
 * **无前缀不匹配任何一条，会落到「按已设置的鉴权变量嗅探」** —— 结果取决于我们注入了
 * 什么，用户无法预期。所以界面上选协议、填裸模型名，前缀由代码拼。
 *
 * 曾经因为这个出过事故：用户把 Base URL 填成智谱的 Anthropic 端点、模型填裸名 glm-5.3，
 * 无前缀被嗅探成 OpenAI，而代码又无条件把 Base URL 写进 OPENAI_BASE_URL，于是请求打到
 *     https://open.bigmodel.cn/api/anthropic/chat/completions
 * 这个不存在的路径上 → 404 NOT_FOUND。
 */
export type ClawProtocol = 'openai' | 'anthropic' | 'xai' | 'dashscope' | 'ollama'

export interface ClawProtocolSpec {
  id: ClawProtocol
  /** 界面上显示的名字。 */
  label: string
  /** claw 内部的模型名前缀。 */
  prefix: string
  /** 该协议的鉴权 env；ollama 不需要 Key。 */
  keyEnv: string | null
  baseUrlEnv: string
  /** 留空 Base URL 时使用；null 表示交给 claw 自己的默认端点。 */
  defaultBaseUrl: string | null
  /** 输入框占位提示用的示例模型，**只是提示，不是限制**。 */
  sampleModel: string
  /** 界面上的一句话说明。 */
  hint: string
}

/**
 * 协议表。字段取值来自 claw 源码的 provider 元数据表；
 * 智谱那两个端点是用真实 Key 打过验证的：
 *   https://open.bigmodel.cn/api/coding/paas/v4   OpenAI 兼容    → 200
 *   https://open.bigmodel.cn/api/anthropic        Anthropic 兼容 → 200
 *   不带 /coding/ 的普通 OpenAI 端点对 Coding Plan 的 Key → 429「无可用资源包」
 *
 * defaultBaseUrl 只是**留空时的缺省**，任何一条都能被用户改掉。
 */
export const CLAW_PROTOCOLS: readonly ClawProtocolSpec[] = [
  {
    id: 'openai',
    label: 'OpenAI 兼容',
    prefix: 'openai/',
    keyEnv: 'OPENAI_API_KEY',
    baseUrlEnv: 'OPENAI_BASE_URL',
    defaultBaseUrl: 'https://open.bigmodel.cn/api/coding/paas/v4',
    sampleModel: 'glm-5.3',
    hint: '任何 OpenAI 兼容服务。留空 Base URL 时用智谱 Coding Plan。'
  },
  {
    id: 'anthropic',
    label: 'Anthropic 协议',
    prefix: 'anthropic/',
    keyEnv: 'ANTHROPIC_API_KEY',
    baseUrlEnv: 'ANTHROPIC_BASE_URL',
    defaultBaseUrl: 'https://open.bigmodel.cn/api/anthropic',
    sampleModel: 'glm-5.3',
    hint: 'Anthropic 官方或任何兼容中转。留空 Base URL 时用智谱的 Anthropic 端点。'
  },
  {
    id: 'xai',
    label: 'xAI（Grok）',
    prefix: 'xai/',
    keyEnv: 'XAI_API_KEY',
    baseUrlEnv: 'XAI_BASE_URL',
    defaultBaseUrl: null,
    sampleModel: 'grok-4',
    hint: '留空 Base URL 时用 xAI 官方端点。'
  },
  {
    id: 'dashscope',
    label: '阿里云百炼 DashScope',
    prefix: 'qwen/',
    keyEnv: 'DASHSCOPE_API_KEY',
    baseUrlEnv: 'DASHSCOPE_BASE_URL',
    defaultBaseUrl: null,
    sampleModel: 'qwen-max',
    hint: '通义千问 / Kimi 走这条。'
  },
  {
    id: 'ollama',
    label: '本地 Ollama',
    prefix: 'local/',
    keyEnv: null,
    baseUrlEnv: 'OLLAMA_HOST',
    defaultBaseUrl: 'http://127.0.0.1:11434/v1',
    sampleModel: 'qwen2.5-coder',
    hint: '连本机模型，不需要 API Key。'
  }
]

export const CLAW_DEFAULT_PROTOCOL: ClawProtocol = 'openai'
export const CLAW_DEFAULT_MODEL = 'glm-5.3'

export function clawProtocolSpec(protocol: ClawProtocol): ClawProtocolSpec {
  return CLAW_PROTOCOLS.find((entry) => entry.id === protocol) ?? CLAW_PROTOCOLS[0]
}

// claw 默认把会话写在 <cwd>/.claw/sessions/ —— 而 cwd 是**用户自己的仓库**，
// 等于我们往人家仓库里拉目录。fork 里给 SessionStore::from_cwd 加了这个 env：
// 设了就改用 <CLAW_DATA_DIR>/sessions/<workspace 指纹>/，不同仓库仍互相隔离。
export const CLAW_DATA_DIR_ENV = 'CLAW_DATA_DIR'

/** 主模型；claw 里 --model 优先于它，所以两边同时给是安全的。 */
export const CLAW_MODEL_ENV = 'CLAW_MODEL'
/** 子代理专用模型；fork 里的回退链第一顺位就是它。 */
export const CLAW_SUBAGENT_MODEL_ENV = 'CLAW_SUBAGENT_MODEL'

export interface ClawManagedConfig {
  /** 协议。老配置里没有这个字段，读取时按模型前缀反推。 */
  protocol: ClawProtocol
  apiKey: string
  /** 空表示用该协议的默认端点。 */
  baseUrl: string
  /** **裸模型名**，不带前缀；前缀由协议决定、代码拼。 */
  model: string
  /**
   * 子代理（claw 的 Agent 工具）用的裸模型名。**留空就跟随主模型**。
   *
   * 上游把它写死成 `claude-opus-4-6`，只对 Anthropic 官方凭据有意义：用智谱 /
   * DashScope / Ollama 跑 claw 时，一旦触发子代理就会拿着用户的 Key 去请求一个
   * 对方根本没有的模型名。fork 里已把那个常量拆掉，改成读下面这组 env。
   */
  subagentModel: string
}

export const EMPTY_CLAW_CONFIG: ClawManagedConfig = {
  protocol: CLAW_DEFAULT_PROTOCOL,
  apiKey: '',
  baseUrl: '',
  model: '',
  subagentModel: ''
}

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
 * 拼出传给 claw 的完整模型名。
 * 用户填裸名；已经带了任一已知前缀就原样保留（方便直接粘贴全名）。
 */
export function clawQualifiedModel(config: ClawManagedConfig): string {
  return qualify(config.model.trim() || CLAW_DEFAULT_MODEL, config.protocol)
}

/**
 * 子代理模型的完整名。**没填就返回 null**，交给 claw 自己回退到主模型——
 * 宿主不在这里替用户拿主意，避免两边各算一份、以后不一致。
 */
export function clawQualifiedSubagentModel(config: ClawManagedConfig): string | null {
  const model = config.subagentModel.trim()
  return model ? qualify(model, config.protocol) : null
}

/** 已经带任一已知前缀就原样保留（方便直接粘贴全名）。 */
function qualify(model: string, protocol: ClawProtocol): string {
  const lower = model.toLowerCase()
  if (CLAW_PROTOCOLS.some((spec) => lower.startsWith(spec.prefix))) return model
  return `${clawProtocolSpec(protocol).prefix}${model}`
}

/** 老配置没有 protocol 字段时按模型前缀反推，保证升级后行为不变。 */
export function clawProtocolFromModel(model: string): ClawProtocol {
  const lower = model.trim().toLowerCase()
  return CLAW_PROTOCOLS.find((spec) => lower.startsWith(spec.prefix))?.id ?? CLAW_DEFAULT_PROTOCOL
}

/** 去掉前缀，还原成界面上显示的裸模型名。 */
export function clawBareModel(model: string): string {
  const trimmed = model.trim()
  const lower = trimmed.toLowerCase()
  const spec = CLAW_PROTOCOLS.find((entry) => lower.startsWith(entry.prefix))
  return spec ? trimmed.slice(spec.prefix.length) : trimmed
}

/**
 * 按所选协议把凭据和端点写进**对应的那一组 env**。
 * 用户在设置里显式写的同名 env 优先——那是他们指向别的服务的出口。
 */
export function withClawManagedEnv(
  command: string,
  env: Record<string, string> | undefined,
  config: ClawManagedConfig
): Record<string, string> | undefined {
  if (!isClawCommand(command)) return env
  const next = { ...(env ?? {}) }
  const spec = clawProtocolSpec(config.protocol)
  // ollama 不需要 Key；其余协议没 Key 就什么都不注入。
  if (spec.keyEnv && !config.apiKey) return next

  if (spec.keyEnv && !next[spec.keyEnv]) next[spec.keyEnv] = config.apiKey
  const baseUrl = config.baseUrl.trim() || spec.defaultBaseUrl
  if (baseUrl && !next[spec.baseUrlEnv]) next[spec.baseUrlEnv] = baseUrl

  // 主模型同时走 --model 和 CLAW_MODEL：claw 的子代理进不到命令行参数，
  // 只能从 env 看到主模型。两者取值相同，且 claw 里 --model 优先于 env。
  if (!next[CLAW_MODEL_ENV]) next[CLAW_MODEL_ENV] = clawQualifiedModel(config)
  const subagentModel = clawQualifiedSubagentModel(config)
  if (subagentModel && !next[CLAW_SUBAGENT_MODEL_ENV]) {
    next[CLAW_SUBAGENT_MODEL_ENV] = subagentModel
  }
  return next
}

/**
 * 把会话目录挪出用户仓库。与凭据分开是刻意的：**没配 API Key 也要生效**——
 * 仓库污染和有没有配置协议无关。
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
 * 补上 `--model`。claw 缺省走 anthropic/claude-*，在我们注入的配置下必然认证失败，
 * 所以没显式指定时要注入带前缀的模型名。
 * 用户自己在 args 里写了 --model 就完全不动。
 */
export function withClawModelArgs(
  command: string,
  args: readonly string[],
  config: ClawManagedConfig
): string[] {
  if (!isClawCommand(command)) return [...args]
  if (args.some((arg) => arg === '--model' || arg.startsWith('--model='))) return [...args]
  const spec = clawProtocolSpec(config.protocol)
  if (spec.keyEnv && !config.apiKey) return [...args]
  return ['--model', clawQualifiedModel(config), ...args]
}
