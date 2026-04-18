import { promises as fs } from 'fs'
import { join, dirname } from 'path'
import { fileURLToPath } from 'url'

const __dirname = dirname(fileURLToPath(import.meta.url))

/** Default artifact path (relative to project dir) per stage. */
export const STAGE_ARTIFACTS: Record<number, string> = {
  1: 'workspaces/stage1_design/design.md',
  2: 'artifacts/impl-summary.md',
  3: 'artifacts/acceptance.md',
  4: 'artifacts/test-report.md'
}

/**
 * Compute the stage's working artifact path. When a plan name is given,
 * Stage 1 uses `<planName>.md` inside its isolated workspace so the file
 * name matches the plan label (instead of a generic "design.md"). Stages
 * 2-4 retain their semantic names (impl-summary/acceptance/test-report)
 * since they represent different document types, not "the plan".
 */
export function stageArtifactPath(stageId: number, label?: string | null): string {
  if (stageId === 1 && label && label.trim()) {
    const safe = label
      .trim()
      .replace(/[<>:"/\\|?*\x00-\x1f]/g, '_')
      .replace(/\s+/g, '_')
      .slice(0, 80)
    return `workspaces/stage1_design/${safe}.md`
  }
  return STAGE_ARTIFACTS[stageId]
}

/**
 * Per-stage cwd (relative to project dir).
 * Stage 1 runs in an isolated empty workspace (codex --full-auto is sandboxed
 * to its cwd subtree and therefore cannot touch source code).
 * Stages 2-4 cd into target_repo (via symlink) since they need to read/run/test code.
 */
export const STAGE_CWD: Record<number, string> = {
  1: 'workspaces/stage1_design',
  2: 'workspaces/stage2_impl',
  3: 'workspaces/stage3_acceptance',
  4: 'workspaces/stage4_test'
}

export const STAGE_NAMES: Record<number, string> = {
  1: '方案设计',
  2: '方案实施',
  3: '方案验收',
  4: '测试验证'
}

/**
 * Per-stage CLI binary.
 *   Stage 1 (方案设计) uses `claude` (Claude Code) — its brainstorming /
 *     writing-plans skills and auto-loaded CLAUDE.md fit the pure
 *     conversation-driven design flow best.
 *   Stage 2 (方案实施) uses `codex` (OpenAI Codex CLI) with --full-auto —
 *     the sandbox allows writing inside cwd subtree (target_repo), which is
 *     exactly the impl stage's scope.
 *   Stage 3 / 4 (验收 / 测试) stay on `claude` — read-heavy, needs MCP/tools.
 */
export const STAGE_COMMAND: Record<number, string> = {
  1: 'claude',
  2: 'codex',
  3: 'claude',
  4: 'claude'
}

/**
 * Safe read-only / inspection commands pre-approved for every Claude stage.
 * `auto` permission mode already auto-judges safety, but explicit allow-listing
 * removes any chance of a prompt for these common operations.
 */
const SAFE_READS = [
  'Read',
  'Glob',
  'Grep',
  'Bash(ls:*)',
  'Bash(cat:*)',
  'Bash(head:*)',
  'Bash(tail:*)',
  'Bash(wc:*)',
  'Bash(find:*)',
  'Bash(pwd)',
  'Bash(echo:*)'
]

const SAFE_GIT = [
  'Bash(git status:*)',
  'Bash(git log:*)',
  'Bash(git diff:*)',
  'Bash(git show:*)',
  'Bash(git blame:*)',
  'Bash(git branch:*)',
  'Bash(git remote:*)',
  'Bash(git rev-parse:*)'
]

const TEST_RUNNERS = [
  'Bash(npm test:*)',
  'Bash(npm run test:*)',
  'Bash(pnpm test:*)',
  'Bash(yarn test:*)',
  'Bash(pytest:*)',
  'Bash(go test:*)',
  'Bash(cargo test:*)',
  'Bash(make test:*)'
]

const BUILD_RUNNERS = [
  'Bash(npm run build:*)',
  'Bash(pnpm build:*)',
  'Bash(yarn build:*)',
  'Bash(make:*)',
  'Bash(docker build:*)',
  'Bash(cargo build:*)',
  'Bash(go build:*)'
]

function claudeArgs(extra: string[] = []): string[] {
  const allowed = [...SAFE_READS, ...SAFE_GIT, ...extra].join(' ')
  return ['--permission-mode', 'auto', '--allowedTools', allowed]
}

/**
 * Per-stage CLI args. Hard "only Stage 2 (方案实施) modifies code" constraint is
 * enforced in role prompts; allow-lists below just spare common safe ops from prompts.
 *
 *   - Claude (stage 1 — design): read-only allowlist; no code/test/build execution.
 *   - Codex  (stage 2 — impl):    --full-auto — sandbox bounded by cwd (target_repo).
 *   - Claude (stages 3-4):         --permission-mode auto + per-stage allowlist.
 */
export const STAGE_CLI_ARGS: Record<number, string[]> = {
  1: claudeArgs(), // design stage: read + git-readonly only
  2: ['--full-auto'], // impl stage: codex sandbox write-in-cwd
  3: claudeArgs(),
  4: claudeArgs([...TEST_RUNNERS, ...BUILD_RUNNERS])
}

function promptsDir(): string {
  // In dev/prod both: prompts files are shipped alongside the compiled main
  // under electron/prompts. In prod they remain in app resources; we ship them
  // via electron-builder "files" glob. Resolve relative to the compiled file.
  return join(__dirname, '..', '..', 'electron', 'prompts')
}

function fallbackPromptsDir(): string {
  // Fallback when running packaged app where source is gone: co-located copy
  return join(__dirname, 'prompts')
}

export async function loadStagePromptTemplate(stageId: number): Promise<string> {
  const names: Record<number, string> = {
    1: 'stage1-design.md',
    2: 'stage2-impl.md',
    3: 'stage3-acceptance.md',
    4: 'stage4-test.md'
  }
  const file = names[stageId]
  if (!file) throw new Error(`unknown stage ${stageId}`)

  for (const base of [promptsDir(), fallbackPromptsDir()]) {
    try {
      return await fs.readFile(join(base, file), 'utf8')
    } catch {
      // try next
    }
  }
  throw new Error(`prompt template not found for stage ${stageId}`)
}

export interface RenderContext {
  projectDir: string
  /** Project-dir-relative artifact path (stable, used by platform to read). */
  artifactPath: string
  projectName?: string
  targetRepo?: string
  stageCwd?: string
}

export function renderTemplate(tpl: string, ctx: RenderContext): string {
  // Pass absolute path to the AI to avoid any cwd-relative ambiguity.
  const artifactAbs = ctx.artifactPath.startsWith('/')
    ? ctx.artifactPath
    : `${ctx.projectDir.replace(/\/$/, '')}/${ctx.artifactPath}`
  return tpl
    .replaceAll('{{PROJECT_DIR}}', ctx.projectDir)
    .replaceAll('{{PROJECT_NAME}}', ctx.projectName ?? '(未设置)')
    .replaceAll('{{TARGET_REPO}}', ctx.targetRepo ?? '(未设置)')
    .replaceAll('{{STAGE_CWD}}', ctx.stageCwd ?? ctx.projectDir)
    .replaceAll('{{ARTIFACT_PATH}}', artifactAbs)
}

function buildProjectContextBlock(ctx: RenderContext): string {
  return [
    '# 项目上下文（平台自动注入，所有阶段共享）',
    '',
    `- **项目名**：${ctx.projectName ?? '(未设置)'}`,
    `- **代码仓库绝对路径**：${ctx.targetRepo ?? '(未设置)'}`,
    `- **你的工作目录 (cwd)**：${ctx.stageCwd ?? ctx.projectDir}`,
    `- **项目根目录**：${ctx.projectDir}`,
    '',
    '---',
    ''
  ].join('\n')
}

export async function buildSystemPrompt(
  stageId: number,
  ctx: RenderContext
): Promise<string> {
  const tpl = await loadStagePromptTemplate(stageId)
  const body = renderTemplate(tpl, ctx)
  return buildProjectContextBlock(ctx) + body
}

export interface HandoffContext {
  fromStage: number
  toStage: number
  artifactPath: string | null
  artifactContent: string | null
  /** Stage 1 design.md content; always bundled when to stage >= 3 for traceability. */
  designSpec?: string | null
  /** Stage 3 acceptance.md content; bundled when advancing 3 → 4 so tester has criteria. */
  acceptanceReport?: string | null
  summary?: string
  verdict?: string
  reason?: string
}

/**
 * Builds the message to feed into the NEXT stage's CC when advancing forward.
 */
export function buildForwardHandoff(h: HandoffContext): string {
  const fromName = STAGE_NAMES[h.fromStage] ?? `Stage ${h.fromStage}`
  const toName = STAGE_NAMES[h.toStage] ?? `Stage ${h.toStage}`
  const lines = [
    `# Handoff: ${fromName} (Stage ${h.fromStage}) → ${toName} (Stage ${h.toStage})`,
    ''
  ]
  if (h.summary) lines.push(`**摘要**: ${h.summary}`, '')
  if (h.verdict) lines.push(`**结论**: ${h.verdict}`, '')

  if (h.artifactContent) {
    lines.push(`## 上一阶段产物 (${h.artifactPath})`, '', h.artifactContent, '')
  } else if (h.artifactPath) {
    lines.push(`上一阶段产物路径: \`${h.artifactPath}\``, '')
  }

  // Provide the original design spec as an authoritative source of truth for
  // every stage that needs to validate against it (acceptance / test).
  if (h.designSpec && h.toStage >= 3) {
    lines.push(
      '## 原始设计文档（Stage 1 产出，作为验收/测试的基准）',
      '',
      h.designSpec,
      ''
    )
  }

  // When entering the test stage, include acceptance report (which should
  // contain the "测试验证标准" section).
  if (h.acceptanceReport && h.toStage === 4) {
    lines.push(
      '## 方案验收报告（Stage 3 产出，包含测试验证标准）',
      '',
      h.acceptanceReport,
      ''
    )
  }

  lines.push(
    '---',
    '',
    '请基于以上信息开始本阶段工作。完成后按系统 prompt 约定输出 `<<STAGE_DONE ...>>` 标记。'
  )
  return lines.join('\n')
}

/**
 * Builds the message to feed into the TARGET stage when user triggers a
 * reverse feedback (e.g. stage 3 → stage 2).
 */
export function buildFeedbackHandoff(params: {
  fromStage: number
  toStage: number
  note: string
  artifactPath?: string
  artifactContent?: string
}): string {
  const lines = [
    `# Feedback from Stage ${params.fromStage} → Stage ${params.toStage}`,
    '',
    '下游阶段发现以下问题，请基于反馈调整后重新产出产物：',
    '',
    params.note,
    ''
  ]
  if (params.artifactContent) {
    lines.push(`## 参考产物 (${params.artifactPath})`, '', params.artifactContent, '')
  }
  lines.push('---', '', '调整完毕后再次输出 `<<STAGE_DONE ...>>` 标记。')
  return lines.join('\n')
}
