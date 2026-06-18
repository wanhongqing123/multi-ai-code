import {
  collectSkillVariables,
  substituteVariables,
  type Skill,
  type SkillStep
} from './skillTypes'

export interface SkillRunDeps {
  /** Send a user message to the active session. Returns ok/error per IPC. */
  sendUser: (sessionId: string, text: string) => Promise<{ ok: boolean; error?: string }>
  /**
   * Subscribe to PTY data chunks. Must return an unsubscribe function. Used
   * by wait-response to detect when the AI has gone quiet for a bit.
   */
  onData: (cb: (evt: { sessionId: string; chunk: string }) => void) => () => void
  /** After the run completes, bump last_used_at via this hook. */
  touchLastUsed?: (skillId: number | string) => Promise<void> | void
  /** Real clock — replaced in tests. */
  now?: () => number
}

export interface SkillRunOptions {
  sessionId: string
  skill: Skill
  vars: Record<string, string>
  /** Default idle window for wait-response when the step doesn't override. */
  defaultWaitMs?: number
  /** Hard ceiling on the whole run, regardless of step waits. */
  maxRunMs?: number
  /** Collect PTY output seen during wait-response steps for pipeline handoff. */
  captureResponse?: boolean
}

export interface SkillRunOutcome {
  ok: boolean
  stepsRun: number
  stepsTotal: number
  failedStepIndex?: number
  failedReason?: string
  capturedResponse?: string
}

const DEFAULT_WAIT_MS = 1500
const DEFAULT_MAX_RUN_MS = 5 * 60 * 1000

function delay(ms: number): Promise<void> {
  return new Promise((r) => setTimeout(r, ms))
}

/**
 * Waits until the PTY for `sessionId` has been silent for `idleMs` or until
 * the per-step timeout elapses, whichever comes first. Returns whether at
 * least one chunk arrived (a noisy "AI started typing" signal) so the caller
 * can distinguish "AI was working" from "no response at all".
 */
async function waitForIdle(
  deps: SkillRunDeps,
  sessionId: string,
  idleMs: number,
  timeoutMs: number,
  captureResponse = false
): Promise<{ sawData: boolean; capturedResponse: string }> {
  const now = deps.now ?? (() => Date.now())
  let lastChunkAt = now()
  let sawData = false
  let capturedResponse = ''
  const unsub = deps.onData((evt) => {
    if (evt.sessionId !== sessionId) return
    sawData = true
    lastChunkAt = now()
    if (captureResponse) capturedResponse += evt.chunk
  })
  try {
    const startedAt = now()
    // Poll-based: every 100ms check (a) global timeout (b) idle window since
    // lastChunkAt. We give the prompt a 250ms head-start so we don't bail
    // before any byte arrives at all.
    const headStart = 250
    await delay(headStart)
    while (true) {
      const t = now()
      if (t - startedAt >= timeoutMs) break
      const sinceLast = t - lastChunkAt
      if (sawData && sinceLast >= idleMs) break
      // If we never saw any data, still treat the global timeout as the bail.
      await delay(100)
    }
  } finally {
    unsub()
  }
  return { sawData, capturedResponse }
}

/**
 * Executes a Skill end-to-end against an active main session. Pure-ish:
 * IPC + clock are dependency-injected so the runner is testable without
 * Electron in scope.
 */
export async function runSkill(
  deps: SkillRunDeps,
  opts: SkillRunOptions
): Promise<SkillRunOutcome> {
  const defaultWait = opts.defaultWaitMs ?? DEFAULT_WAIT_MS
  const maxRun = opts.maxRunMs ?? DEFAULT_MAX_RUN_MS
  const now = deps.now ?? (() => Date.now())
  const startedAt = now()
  let stepsRun = 0
  let capturedResponse = ''
  const steps = opts.skill.steps

  for (let i = 0; i < steps.length; i++) {
    if (now() - startedAt >= maxRun) {
      return {
        ok: false,
        stepsRun,
        stepsTotal: steps.length,
        failedStepIndex: i,
        failedReason: `total run exceeded ${maxRun}ms`
      }
    }
    const step = steps[i]
    const result = await executeStep(
      deps,
      opts.sessionId,
      step,
      opts.vars,
      defaultWait,
      opts.captureResponse ?? false
    )
    capturedResponse += result.capturedResponse
    stepsRun++
    if (result.failure) {
      return {
        ok: false,
        stepsRun,
        stepsTotal: steps.length,
        failedStepIndex: i,
        failedReason: result.failure,
        capturedResponse: capturedResponse || undefined
      }
    }
  }

  try {
    if (deps.touchLastUsed) await deps.touchLastUsed(opts.skill.id)
  } catch {
    /* non-fatal */
  }
  return {
    ok: true,
    stepsRun,
    stepsTotal: steps.length,
    capturedResponse: capturedResponse || undefined
  }
}

async function executeStep(
  deps: SkillRunDeps,
  sessionId: string,
  step: SkillStep,
  vars: Record<string, string>,
  defaultWait: number,
  captureResponse: boolean
): Promise<{ failure: string | null; capturedResponse: string }> {
  if (step.type === 'prompt') {
    const text = substituteVariables(step.text, vars)
    const res = await deps.sendUser(sessionId, text)
    if (!res.ok) return { failure: res.error ?? 'sendUser failed', capturedResponse: '' }
    return { failure: null, capturedResponse: '' }
  }
  if (step.type === 'wait-response') {
    const idleMs = defaultWait
    const timeoutMs = step.timeoutMs ?? 30_000
    const result = await waitForIdle(deps, sessionId, idleMs, timeoutMs, captureResponse)
    return { failure: null, capturedResponse: result.capturedResponse }
  }
  return {
    failure: `unknown step type: ${(step as { type?: string }).type ?? 'undefined'}`,
    capturedResponse: ''
  }
}

/**
 * Pre-flight helper: returns the list of `{var}` placeholders the user must
 * supply before this skill can run. The runner itself tolerates missing
 * vars (leaves them literal), but the UI should ask first.
 */
export function requiredVariables(skill: Skill): string[] {
  return collectSkillVariables(skill.steps)
}
