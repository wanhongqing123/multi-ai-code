import { spawn } from 'child_process'
import { promises as fs } from 'fs'
import { join } from 'path'
import type { AggregatedCluster } from './aggregator.js'
import type { GenerateFn } from './scheduler.js'
import { projectDir as projectDirFn } from '../store/paths.js'
import { getDb } from '../store/db.js'
import { buildEnvWithPath, resolveCliSpawn } from './cliSpawn.js'
import { withOpenCodeLspEnv } from '../aicli/opencodeConfig.js'
import type { OpenCodeProviderProfile } from '../aicli/opencodeConfig.js'
import { isValidStep, type SkillStep } from './skills.js'
import { enqueueCliJob } from '../util/cliQueue.js'

export interface SkillTemplate {
  title: string
  /**
   * Multi-step recipe. When this is non-empty it is the canonical form;
   * `body` is left undefined for new generations.
   */
  steps?: SkillStep[]
  /**
   * Legacy single-prompt body — kept so candidate rows generated before the
   * Phase-A redesign still render. New generations should populate `steps`.
   */
  body?: string
  /** Short keyword the user can type in the SkillBar to trigger this skill. */
  trigger?: string
  meta?: {
    variables?: string[]
    category?: string
    rationale?: string
    source?: 'cli' | 'heuristic'
  }
}

export const HABIT_KIND_NAMES_ZH: Record<string, string> = {
  pty_cmd: '主会话终端命令',
  ai_prompt_main: '主会话 AI prompt',
  ai_prompt_repo: '仓库查看 AI prompt',
  diff_annotation: 'Diff 批注',
  repo_view_annotation: '仓库查看代码标注',
  template_used: '模板调用',
  plan_imported: '方案导入'
}

/**
 * Builds the structured prompt for the CLI generator. **Privacy-critical**:
 * this is the *only* place data leaves the local DB. We pass:
 *  - high-level cluster description (kind, size, time span, project count)
 *  - up to 5 representative samples (text only, no project_id, no repo_path)
 *  - a strict JSON return format
 *
 * Anything else stays local. Tested separately to enforce these invariants.
 */
export function buildGenerationPrompt(cluster: AggregatedCluster): string {
  const dayMs = 24 * 60 * 60 * 1000
  const spanDays = Math.max(0, Math.round((cluster.lastTs - cluster.firstTs) / dayMs))
  const kindZh = HABIT_KIND_NAMES_ZH[cluster.kind] ?? cluster.kind
  const sampleBlock = cluster.representativeSamples
    .slice(0, 5)
    .map((s, i) => `${i + 1}. ${s.trim()}`)
    .join('\n')

  return [
    '我是一个 AI 工作平台，正在为用户自动建议一个 **可复用的 Skill**（一段可重放的多步骤工作流）。',
    '以下是用户最近重复出现的一类操作。请提炼成一个分步骤的 Skill。',
    '',
    `[簇描述]`,
    `- 类型: ${kindZh} (${cluster.kind})`,
    `- 重复次数: ${cluster.size}`,
    `- 时间跨度: 最近 ${spanDays} 天`,
    `- 涉及项目数: ${cluster.projectCount}`,
    `- 是否跨项目通用: ${cluster.crossProject ? '是' : '否'}`,
    '',
    `[代表样本]`,
    sampleBlock,
    '',
    '[要求]',
    '只返回如下严格 JSON 格式，不要有任何额外说明文字、不要 Markdown 代码围栏：',
    '{"title":"...","trigger":"...","steps":[{"type":"prompt","text":"..."}],"variables":["..."],"category":"...","rationale":"..."}',
    '',
    '字段说明：',
    '- title: 不超过 20 字的 Skill 名称',
    '- trigger: 用户在主会话输入条里能快速联想到的短关键词（如 "审查改动" / "看实现"），1~6 字',
    '- steps: 数组，按顺序执行。每个元素是 step 对象：',
    '  - {"type":"prompt","text":"..."} ：发送一段提示给主会话；text 中用 {变量名} 标记每次需要用户填的位置',
    '  - {"type":"wait-response"} 或 {"type":"wait-response","timeoutMs":30000} ：等 AI 答完再走下一个 prompt',
    '- variables: 所有 step.text 里出现的变量名',
    '- category: 用途分类（如 "代码审查" / "解释代码" / "改 bug"）',
    '- rationale: 一句话说明为什么这个 Skill 有用',
    '',
    '指导原则：',
    '- 如果用户的重复操作天然是 "问 → 等 → 接着问"，就拆成多个 prompt + wait-response',
    '- 如果只是单次提问，steps 可以只有一个 prompt 元素',
    '- 不要超过 4 个 step；保持紧凑'
  ].join('\n')
}

const FENCE_RE = /```(?:json)?\s*([\s\S]*?)\s*```/i

export interface ParsedResponse {
  ok: boolean
  template?: SkillTemplate
  error?: string
}

/**
 * Tolerantly parses the CLI response. Accepts:
 *  - bare JSON
 *  - JSON wrapped in a ```...``` fence
 *  - JSON with surrounding chatter (extracts the first `{...}` block)
 */
export function parseGenerationResponse(raw: string): ParsedResponse {
  if (!raw || typeof raw !== 'string') {
    return { ok: false, error: 'empty response' }
  }
  let candidate = raw.trim()
  const fenced = candidate.match(FENCE_RE)
  if (fenced && fenced[1]) candidate = fenced[1].trim()
  if (!candidate.startsWith('{')) {
    const first = candidate.indexOf('{')
    const last = candidate.lastIndexOf('}')
    if (first >= 0 && last > first) candidate = candidate.slice(first, last + 1)
  }
  let parsed: unknown
  try {
    parsed = JSON.parse(candidate)
  } catch (err) {
    return {
      ok: false,
      error: `failed to parse JSON: ${(err as Error).message}`
    }
  }
  if (!parsed || typeof parsed !== 'object') {
    return { ok: false, error: 'response is not an object' }
  }
  const obj = parsed as Record<string, unknown>
  const title = typeof obj.title === 'string' ? obj.title.trim() : ''
  if (!title) return { ok: false, error: 'response missing title' }

  // New multi-step shape takes precedence.
  let steps: SkillStep[] | undefined
  if (Array.isArray(obj.steps)) {
    steps = obj.steps.filter(isValidStep)
    if (steps.length === 0) steps = undefined
  }
  // Legacy single-body shape — wrap into a single prompt step so downstream
  // code can treat new and old candidates uniformly.
  const legacyBody = typeof obj.body === 'string' ? obj.body : ''
  if (!steps && legacyBody) {
    steps = [{ type: 'prompt', text: legacyBody }]
  }
  if (!steps || steps.length === 0) {
    return { ok: false, error: 'response has no steps and no body' }
  }

  const variables = Array.isArray(obj.variables)
    ? obj.variables.filter((v): v is string => typeof v === 'string')
    : []
  const category = typeof obj.category === 'string' ? obj.category : undefined
  const rationale = typeof obj.rationale === 'string' ? obj.rationale : undefined
  const trigger = typeof obj.trigger === 'string' && obj.trigger.trim().length > 0
    ? obj.trigger.trim()
    : undefined
  return {
    ok: true,
    template: {
      title,
      steps,
      // Keep body populated for backward-compat consumers (legacy candidate
      // rows that still display a single body field).
      body: steps.length === 1 && steps[0].type === 'prompt' ? steps[0].text : undefined,
      trigger,
      meta: { variables, category, rationale, source: 'cli' }
    }
  }
}

/**
 * Fully local fallback: produces a candidate without invoking any LLM. It
 * picks the longest representative sample as the body and derives a title
 * from its first chunk. Output quality is lower than CLI-generated, but it
 * ensures the feature works offline / with no CLI configured.
 */
export function buildLocalHeuristicCandidate(cluster: AggregatedCluster): SkillTemplate {
  const samples = cluster.representativeSamples.filter((s) => s.trim().length > 0)
  const seed = samples.length > 0 ? samples.slice().sort((a, b) => b.length - a.length)[0] : ''
  const trimmed = seed.replace(/\s+/g, ' ').trim()
  const title = trimmed.length === 0
    ? `${HABIT_KIND_NAMES_ZH[cluster.kind] ?? cluster.kind} 模板`
    : trimmed.length <= 18
      ? trimmed
      : trimmed.slice(0, 18) + '…'
  const trigger = title.replace(/[…\s]+$/g, '').slice(0, 6)
  // Heuristic can't actually invent a multi-step recipe without LLM smarts,
  // so we wrap the representative sample as a single prompt step. Once the
  // user runs it, they can edit the skill to split it into more steps.
  const steps: SkillStep[] = seed
    ? [{ type: 'prompt', text: seed }]
    : []
  return {
    title,
    steps,
    body: seed || undefined,
    trigger: trigger || undefined,
    meta: {
      variables: [],
      category: HABIT_KIND_NAMES_ZH[cluster.kind] ?? cluster.kind,
      rationale: `本地启发：基于最近 ${cluster.size} 次相似操作`,
      source: 'heuristic'
    }
  }
}

interface RawAiSettings {
  ai_cli?: 'claude' | 'codex' | 'opencode'
  command?: string
  args?: string[]
  env?: Record<string, string>
  opencode?: OpenCodeProviderProfile
}

/**
 * Reads the AI settings for the most-recently-used project (top of the
 * projects table by updated_at). Returns null if no project has settings.
 */
async function loadDefaultAiSettings(): Promise<RawAiSettings | null> {
  try {
    const row = getDb()
      .prepare(`SELECT id FROM projects ORDER BY updated_at DESC LIMIT 1`)
      .get() as { id?: string } | undefined
    if (!row?.id) return null
    const metaPath = join(projectDirFn(row.id), 'project.json')
    const raw = await fs.readFile(metaPath, 'utf8')
    const meta = JSON.parse(raw) as { ai_settings?: RawAiSettings }
    return meta.ai_settings ?? null
  } catch {
    return null
  }
}

/** Hard cap on CLI subprocess wall time. */
export const CLI_TIMEOUT_MS = 60 * 1000
export const CODEX_CONTEXT_WINDOW_CONFIG = 'model_context_window=1000000'

function hasCodexContextWindowConfig(args: readonly string[]): boolean {
  return args.some((arg, index) => {
    if (arg === '-c' || arg === '--config') {
      return args[index + 1]?.startsWith('model_context_window=') === true
    }
    return arg.startsWith('-cmodel_context_window=') ||
      arg.startsWith('--config=model_context_window=')
  })
}

function codexDefaultArgs(extraArgs: readonly string[]): string[] {
  return hasCodexContextWindowConfig(extraArgs)
    ? []
    : ['-c', CODEX_CONTEXT_WINDOW_CONFIG]
}

function opencodeDefaultArgs(extraArgs: readonly string[]): string[] {
  return extraArgs.some((arg) =>
    ['--dangerously-skip-permissions', '--yolo', '--auto'].includes(arg)
  )
    ? []
    : ['--dangerously-skip-permissions']
}

export function buildCliArgs(
  settings: RawAiSettings,
  prompt: string
): { cmd: string; args: string[] } {
  const cli = settings.ai_cli ?? 'codex'
  const cmd = settings.command ?? cli
  const extras = settings.args ?? []
  if (cli === 'codex') {
    return { cmd, args: ['exec', ...codexDefaultArgs(extras), ...extras, prompt] }
  }
  if (cli === 'opencode') {
    return { cmd, args: ['run', ...opencodeDefaultArgs(extras), ...extras, prompt] }
  }
  return { cmd, args: ['-p', ...extras, prompt] }
}

/**
 * Spawns the CLI in non-interactive mode and captures stdout. Cross-platform:
 *   - Resolves the binary via PATH + PATHEXT (Windows) or PATH (Mac/Linux)
 *   - Wraps Windows .cmd/.bat shims through cmd.exe (no `shell: true`)
 *   - Augments PATH with /opt/homebrew/bin & /usr/local/bin on macOS so brew
 *     installs resolve when Electron is launched outside a user shell
 *
 * Errors, timeouts, and non-zero exit codes are surfaced as ParsedResponse
 * so the caller can fall back to local heuristics.
 */
export function runCliGeneration(
  prompt: string,
  settings: RawAiSettings,
  opts: { timeoutMs?: number } = {}
): Promise<ParsedResponse> {
  const { cmd, args } = buildCliArgs(settings, prompt)
  const env = withOpenCodeLspEnv(
    cmd,
    buildEnvWithPath({
      ...process.env,
      ...(settings.env ?? {})
    }),
    settings.opencode
  ) ?? {}
  const timeoutMs = opts.timeoutMs ?? CLI_TIMEOUT_MS

  const resolution = resolveCliSpawn(cmd, args, env)
  if (!resolution.ok) {
    return Promise.resolve({ ok: false, error: resolution.error })
  }
  const { spawnCommand, spawnArgs, launchNotice } = resolution.resolved
  if (launchNotice) {
    console.info(`[aicli] ${launchNotice}`)
  }

  return new Promise<ParsedResponse>((resolve) => {
    let proc
    try {
      proc = spawn(spawnCommand, spawnArgs, { env, shell: false })
    } catch (err) {
      resolve({
        ok: false,
        error: `failed to spawn ${spawnCommand}: ${(err as Error).message}`
      })
      return
    }
    let stdout = ''
    let stderr = ''
    let settled = false
    const settle = (result: ParsedResponse): void => {
      if (settled) return
      settled = true
      try {
        proc.kill()
      } catch {
        /* ignore */
      }
      resolve(result)
    }
    proc.stdout?.on('data', (b: Buffer) => {
      stdout += b.toString('utf8')
    })
    proc.stderr?.on('data', (b: Buffer) => {
      stderr += b.toString('utf8')
    })
    proc.on('error', (err) => {
      settle({ ok: false, error: `spawn error: ${err.message}` })
    })
    proc.on('close', (code) => {
      if (code !== 0) {
        settle({
          ok: false,
          error: `${cmd} exited ${code}: ${stderr.slice(0, 500)}`
        })
        return
      }
      settle(parseGenerationResponse(stdout))
    })
    const timer = setTimeout(() => {
      settle({ ok: false, error: `${cmd} timed out after ${timeoutMs}ms` })
    }, timeoutMs)
    proc.on('close', () => clearTimeout(timer))
  })
}

/**
 * Creates the GenerateFn used by the scheduler. Tries the CLI first; on
 * failure (no CLI configured, subprocess error, timeout, malformed JSON),
 * falls back to the local heuristic so the user always gets *something*.
 */
/** Internal: lift a SkillTemplate into the scheduler's GenerateFn result shape. */
function templateToGenerateResult(t: SkillTemplate): {
  ok: true
  title: string
  body?: string
  steps?: SkillStep[]
  trigger?: string
  meta?: unknown
} {
  return {
    ok: true as const,
    title: t.title,
    body: t.body,
    steps: t.steps,
    trigger: t.trigger,
    meta: t.meta
  }
}

export function createDefaultSkillGenerator(): GenerateFn {
  return async (cluster: AggregatedCluster) => {
    const settings = await loadDefaultAiSettings()
    if (settings) {
      try {
        const prompt = buildGenerationPrompt(cluster)
        // Funnel through the shared CLI queue so the KB summarizer can't
        // race for the same OAuth token / rate-limit budget.
        const result = await enqueueCliJob('habit-skill-gen', () =>
          runCliGeneration(prompt, settings)
        )
        if (result.ok && result.template) {
          return templateToGenerateResult(result.template)
        }
        // CLI tried and failed — keep the error around as part of the heuristic
        // candidate's rationale so the user can see why we fell back.
        const heuristic = buildLocalHeuristicCandidate(cluster)
        return templateToGenerateResult({
          ...heuristic,
          meta: {
            ...heuristic.meta,
            rationale:
              (heuristic.meta?.rationale ?? '') +
              `（CLI 生成失败：${result.error ?? '未知错误'}，已使用本地启发兜底）`
          }
        })
      } catch (err) {
        const heuristic = buildLocalHeuristicCandidate(cluster)
        return templateToGenerateResult({
          ...heuristic,
          meta: {
            ...heuristic.meta,
            rationale:
              (heuristic.meta?.rationale ?? '') +
              `（CLI 调用异常：${(err as Error).message}）`
          }
        })
      }
    }
    // No AI settings available at all — pure local heuristic.
    return templateToGenerateResult(buildLocalHeuristicCandidate(cluster))
  }
}
