import { spawn } from 'child_process'
import { promises as fs } from 'fs'
import { join } from 'path'
import type { AggregatedCluster } from './aggregator.js'
import type { GenerateFn } from './scheduler.js'
import { projectDir as projectDirFn } from '../store/paths.js'
import { getDb } from '../store/db.js'
import { buildEnvWithPath, resolveCliSpawn } from './cliSpawn.js'

export interface SkillTemplate {
  title: string
  body: string
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
    '我是一个 AI 工作平台，正在为用户自动建议 prompt 模板。',
    '以下是用户最近重复出现的一类操作。请基于这些样本提炼出一个可复用的 prompt 模板。',
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
    '基于以上模式，生成一个可复用的 prompt 模板。',
    '只返回如下严格 JSON 格式，不要有任何额外说明文字、不要 Markdown 代码围栏：',
    '{"title":"...","body":"...","variables":["..."],"category":"...","rationale":"..."}',
    '说明：',
    '- title: 不超过 20 个字符的模板名',
    '- body: prompt 主体，用 {变量名} 占位用户每次需要填的位置',
    '- variables: body 中出现的变量名列表',
    '- category: 模板分类（如 "代码审查" / "解释代码" / "改 bug"）',
    '- rationale: 一句话说明这个模板为什么有用'
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
  const body = typeof obj.body === 'string' ? obj.body : ''
  if (!title || !body) {
    return { ok: false, error: 'response missing title or body' }
  }
  const variables = Array.isArray(obj.variables)
    ? obj.variables.filter((v): v is string => typeof v === 'string')
    : []
  const category = typeof obj.category === 'string' ? obj.category : undefined
  const rationale = typeof obj.rationale === 'string' ? obj.rationale : undefined
  return {
    ok: true,
    template: {
      title,
      body,
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
  return {
    title,
    body: seed,
    meta: {
      variables: [],
      category: HABIT_KIND_NAMES_ZH[cluster.kind] ?? cluster.kind,
      rationale: `本地启发：基于最近 ${cluster.size} 次相似操作`,
      source: 'heuristic'
    }
  }
}

interface RawAiSettings {
  ai_cli?: 'claude' | 'codex'
  command?: string
  args?: string[]
  env?: Record<string, string>
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

export function buildCliArgs(
  settings: RawAiSettings,
  prompt: string
): { cmd: string; args: string[] } {
  const cli = settings.ai_cli ?? 'claude'
  const cmd = settings.command ?? cli
  const extras = settings.args ?? []
  if (cli === 'codex') {
    return { cmd, args: ['exec', ...extras, prompt] }
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
  const env = buildEnvWithPath({
    ...process.env,
    ...(settings.env ?? {})
  })
  const timeoutMs = opts.timeoutMs ?? CLI_TIMEOUT_MS

  const resolution = resolveCliSpawn(cmd, args, env)
  if (!resolution.ok) {
    return Promise.resolve({ ok: false, error: resolution.error })
  }
  const { spawnCommand, spawnArgs } = resolution.resolved

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
export function createDefaultSkillGenerator(): GenerateFn {
  return async (cluster: AggregatedCluster) => {
    const settings = await loadDefaultAiSettings()
    if (settings) {
      try {
        const prompt = buildGenerationPrompt(cluster)
        const result = await runCliGeneration(prompt, settings)
        if (result.ok && result.template) {
          return {
            ok: true as const,
            title: result.template.title,
            body: result.template.body,
            meta: result.template.meta
          }
        }
        // CLI tried and failed — keep the error around as part of the heuristic
        // candidate's rationale so the user can see why we fell back.
        const heuristic = buildLocalHeuristicCandidate(cluster)
        return {
          ok: true as const,
          title: heuristic.title,
          body: heuristic.body,
          meta: {
            ...heuristic.meta,
            rationale:
              (heuristic.meta?.rationale ?? '') +
              `（CLI 生成失败：${result.error ?? '未知错误'}，已使用本地启发兜底）`
          }
        }
      } catch (err) {
        const heuristic = buildLocalHeuristicCandidate(cluster)
        return {
          ok: true as const,
          title: heuristic.title,
          body: heuristic.body,
          meta: {
            ...heuristic.meta,
            rationale:
              (heuristic.meta?.rationale ?? '') +
              `（CLI 调用异常：${(err as Error).message}）`
          }
        }
      }
    }
    // No AI settings available at all — pure local heuristic.
    const heuristic = buildLocalHeuristicCandidate(cluster)
    return {
      ok: true as const,
      title: heuristic.title,
      body: heuristic.body,
      meta: heuristic.meta
    }
  }
}
