import { randomUUID } from 'crypto'
import { isAbsolute } from 'path'
import { readProjectMetaFile, writeProjectMetaFile } from '../store/projectMeta.js'
import type {
  BuildOutputEncoding,
  BuildStepConfig,
  BuildStepEnvType,
  ProjectBuildConfig,
} from './types.js'

export type { BuildOutputEncoding, BuildStepConfig, BuildStepEnvType, ProjectBuildConfig } from './types.js'

export interface BuildConfigValidationIssue {
  path: string
  message: string
}

export type ProjectBuildConfigReadResult =
  | { ok: true; value: ProjectBuildConfig; repaired?: true }
  | { ok: false; error: string }

export type ProjectBuildConfigWriteResult =
  | { ok: true; repaired?: true }
  | { ok: false; error: string; details?: BuildConfigValidationIssue[] }

export const DEFAULT_BUILD_CONFIG: ProjectBuildConfig = { enabled: false, steps: [] }

interface NormalizeBuildConfigOptions {
  createId?: () => string
}

interface NormalizedBuildStep extends Omit<BuildStepConfig, 'visualStudioInstanceId' | 'outputEncoding'> {
  rawEnvType?: unknown
  rawOutputEncoding?: unknown
  visualStudioInstanceId: string
  outputEncoding: BuildOutputEncoding
}

interface NormalizedBuildConfig {
  enabled: boolean
  steps: NormalizedBuildStep[]
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return !!value && typeof value === 'object' && !Array.isArray(value)
}

function isBuildStepEnvType(value: unknown): value is BuildStepEnvType {
  return value === 'system' || value === 'msys' || value === 'visual-studio'
}

function isBuildOutputEncoding(value: unknown): value is BuildOutputEncoding {
  return value === 'auto' || value === 'utf8' || value === 'gbk'
}

function hasParentTraversal(cwd: string): boolean {
  return cwd.split(/[\\/]+/).some((segment) => segment === '..')
}

function isAbsolutePath(cwd: string): boolean {
  return isAbsolute(cwd) || /^[A-Za-z]:[\\/]/.test(cwd) || /^\\\\/.test(cwd)
}

function normalizeBuildStep(
  value: unknown,
  options?: NormalizeBuildConfigOptions
): NormalizedBuildStep | null {
  if (!isRecord(value)) return null
  const id = typeof value.id === 'string' ? value.id.trim() : ''
  const name = typeof value.name === 'string' ? value.name.trim() : ''
  const cwd = typeof value.cwd === 'string' ? value.cwd.trim() : ''
  const command = typeof value.command === 'string' ? value.command.trim() : ''
  const enabled = typeof value.enabled === 'boolean' ? value.enabled : false
  const envType = isBuildStepEnvType(value.envType) ? value.envType : 'msys'
  const visualStudioInstanceId =
    typeof value.visualStudioInstanceId === 'string' ? value.visualStudioInstanceId.trim() : ''
  const outputEncoding = value.outputEncoding === 'utf8' || value.outputEncoding === 'gbk'
    ? value.outputEncoding
    : 'auto'

  return {
    id: id || (options?.createId ?? randomUUID)(),
    name,
    envType,
    cwd,
    command,
    enabled,
    visualStudioInstanceId,
    outputEncoding,
    rawEnvType: isBuildStepEnvType(value.envType) ? undefined : value.envType,
    rawOutputEncoding: isBuildOutputEncoding(value.outputEncoding) ? undefined : value.outputEncoding,
  }
}

function normalizeBuildConfigInternal(
  value: unknown,
  options?: NormalizeBuildConfigOptions
): NormalizedBuildConfig {
  if (!isRecord(value) || !Array.isArray(value.steps)) {
    return { enabled: false, steps: [] }
  }

  const steps: NormalizedBuildStep[] = []
  for (const step of value.steps) {
    const normalizedStep = normalizeBuildStep(step, options)
    if (normalizedStep) {
      steps.push(normalizedStep)
    }
  }

  return {
    enabled: typeof value.enabled === 'boolean' ? value.enabled : false,
    steps,
  }
}

export function normalizeBuildConfig(
  value: unknown,
  options?: NormalizeBuildConfigOptions
): ProjectBuildConfig {
  const normalized = normalizeBuildConfigInternal(value, options)
  return {
    enabled: normalized.enabled,
    steps: normalized.steps.map(
      ({ rawEnvType: _rawEnvType, rawOutputEncoding: _rawOutputEncoding, ...step }) => step
    ),
  }
}

function validateBuildStep(step: NormalizedBuildStep, index: number): BuildConfigValidationIssue[] {
  const issues: BuildConfigValidationIssue[] = []
  if (!step.id.trim()) {
    issues.push({ path: `build_config.steps[${index}].id`, message: 'id must be a non-empty string' })
  }
  if (!step.name.trim()) {
    issues.push({ path: `build_config.steps[${index}].name`, message: 'name must be a non-empty string' })
  }
  if (step.rawEnvType !== undefined) {
    issues.push({
      path: `build_config.steps[${index}].envType`,
      message: 'envType must be one of: system, msys, visual-studio',
    })
  }
  if (step.envType === 'visual-studio' && !step.visualStudioInstanceId.trim()) {
    issues.push({
      path: `build_config.steps[${index}].visualStudioInstanceId`,
      message: 'visualStudioInstanceId must be selected for visual-studio steps',
    })
  }
  if (step.rawOutputEncoding !== undefined) {
    issues.push({
      path: `build_config.steps[${index}].outputEncoding`,
      message: 'outputEncoding must be one of: auto, utf8, gbk',
    })
  }
  if (!step.cwd.trim()) {
    issues.push({ path: `build_config.steps[${index}].cwd`, message: 'cwd must be a non-empty string' })
  } else if (isAbsolutePath(step.cwd)) {
    issues.push({
      path: `build_config.steps[${index}].cwd`,
      message: 'cwd must be a relative path within target_repo',
    })
  } else if (hasParentTraversal(step.cwd)) {
    issues.push({
      path: `build_config.steps[${index}].cwd`,
      message: 'cwd must not contain parent traversal segments',
    })
  }
  if (!step.command.trim()) {
    issues.push({
      path: `build_config.steps[${index}].command`,
      message: 'command must be a non-empty string',
    })
  }
  return issues
}

function validateBuildConfig(config: NormalizedBuildConfig): BuildConfigValidationIssue[] {
  const issues: BuildConfigValidationIssue[] = []
  for (const [index, step] of config.steps.entries()) {
    issues.push(...validateBuildStep(step, index))
  }
  return issues
}

export async function getProjectBuildConfig(
  metaPath: string
): Promise<ProjectBuildConfigReadResult> {
  try {
    const readResult = await readProjectMetaFile(metaPath)
    if (!readResult.ok) {
      return { ok: false, error: readResult.error }
    }

    const value = normalizeBuildConfig(readResult.meta.build_config)
    return readResult.repaired ? { ok: true, repaired: true, value } : { ok: true, value }
  } catch (error: unknown) {
    return { ok: false, error: error instanceof Error ? error.message : String(error) }
  }
}

export async function setProjectBuildConfig(
  metaPath: string,
  config: ProjectBuildConfig
): Promise<ProjectBuildConfigWriteResult> {
  try {
    const readResult = await readProjectMetaFile(metaPath)
    if (!readResult.ok) {
      return { ok: false, error: readResult.error }
    }

    const normalized = normalizeBuildConfigInternal(config)
    const issues = validateBuildConfig(normalized)
    if (issues.length > 0) {
      return { ok: false, error: 'invalid build config', details: issues }
    }

    await writeProjectMetaFile(metaPath, {
      ...readResult.meta,
      build_config: normalizeBuildConfig(normalized) as unknown as Record<string, unknown>,
    })
    return readResult.repaired ? { ok: true, repaired: true } : { ok: true }
  } catch (error: unknown) {
    return { ok: false, error: error instanceof Error ? error.message : String(error) }
  }
}
