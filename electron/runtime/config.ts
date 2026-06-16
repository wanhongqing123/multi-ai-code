import { isAbsolute } from 'path'
import { readProjectMetaFile, writeProjectMetaFile } from '../store/projectMeta.js'
import type {
  ProjectRuntimeConfig,
  RuntimeEnvType,
  RuntimeOutputEncoding,
} from './types.js'

export type {
  ProjectRuntimeConfig,
  RuntimeEnvType,
  RuntimeOutputEncoding,
} from './types.js'

export interface RuntimeConfigValidationIssue {
  path: string
  message: string
}

export type ProjectRuntimeConfigReadResult =
  | { ok: true; value: ProjectRuntimeConfig; repaired?: true }
  | { ok: false; error: string }

export type ProjectRuntimeConfigWriteResult =
  | { ok: true; repaired?: true }
  | { ok: false; error: string; details?: RuntimeConfigValidationIssue[] }

export const DEFAULT_RUNTIME_CONFIG: ProjectRuntimeConfig = {
  enabled: false,
  cwd: '.',
  command: '',
  envType: 'msys',
  visualStudioInstanceId: '',
  outputEncoding: 'auto',
}

interface NormalizedRuntimeConfig extends ProjectRuntimeConfig {
  rawEnvType?: unknown
  rawOutputEncoding?: unknown
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return !!value && typeof value === 'object' && !Array.isArray(value)
}

function isRuntimeEnvType(value: unknown): value is RuntimeEnvType {
  return value === 'msys' || value === 'visual-studio'
}

function isRuntimeOutputEncoding(value: unknown): value is RuntimeOutputEncoding {
  return value === 'auto' || value === 'utf8' || value === 'gbk'
}

function hasParentTraversal(cwd: string): boolean {
  return cwd.split(/[\\/]+/).some((segment) => segment === '..')
}

function normalizeRuntimeConfigInternal(value: unknown): NormalizedRuntimeConfig {
  if (!isRecord(value)) return { ...DEFAULT_RUNTIME_CONFIG }

  const envType = isRuntimeEnvType(value.envType) ? value.envType : 'msys'
  const outputEncoding = isRuntimeOutputEncoding(value.outputEncoding)
    ? value.outputEncoding
    : 'auto'

  return {
    enabled: typeof value.enabled === 'boolean' ? value.enabled : false,
    cwd: typeof value.cwd === 'string' && value.cwd.trim() ? value.cwd.trim() : '.',
    command: typeof value.command === 'string' ? value.command.trim() : '',
    envType,
    visualStudioInstanceId:
      typeof value.visualStudioInstanceId === 'string'
        ? value.visualStudioInstanceId.trim()
        : '',
    outputEncoding,
    rawEnvType: isRuntimeEnvType(value.envType) ? undefined : value.envType,
    rawOutputEncoding: isRuntimeOutputEncoding(value.outputEncoding)
      ? undefined
      : value.outputEncoding,
  }
}

export function normalizeRuntimeConfig(value: unknown): ProjectRuntimeConfig {
  const {
    rawEnvType: _rawEnvType,
    rawOutputEncoding: _rawOutputEncoding,
    ...config
  } = normalizeRuntimeConfigInternal(value)
  return config
}

function validateRuntimeConfig(config: NormalizedRuntimeConfig): RuntimeConfigValidationIssue[] {
  const issues: RuntimeConfigValidationIssue[] = []

  if (config.rawEnvType !== undefined) {
    issues.push({
      path: 'runtime_config.envType',
      message: 'envType must be one of: msys, visual-studio',
    })
  }
  if (config.rawOutputEncoding !== undefined) {
    issues.push({
      path: 'runtime_config.outputEncoding',
      message: 'outputEncoding must be one of: auto, utf8, gbk',
    })
  }
  if (!config.cwd.trim()) {
    issues.push({ path: 'runtime_config.cwd', message: 'cwd must be a non-empty string' })
  } else if (isAbsolute(config.cwd)) {
    issues.push({
      path: 'runtime_config.cwd',
      message: 'cwd must be a relative path within target_repo',
    })
  } else if (hasParentTraversal(config.cwd)) {
    issues.push({
      path: 'runtime_config.cwd',
      message: 'cwd must not contain parent traversal segments',
    })
  }

  if (config.enabled && config.envType === 'visual-studio' && !config.visualStudioInstanceId) {
    issues.push({
      path: 'runtime_config.visualStudioInstanceId',
      message: 'visualStudioInstanceId must be selected for visual-studio runtime',
    })
  }
  if (config.enabled && !config.command.trim()) {
    issues.push({
      path: 'runtime_config.command',
      message: 'command must be a non-empty string',
    })
  }

  return issues
}

export async function getProjectRuntimeConfig(
  metaPath: string
): Promise<ProjectRuntimeConfigReadResult> {
  try {
    const readResult = await readProjectMetaFile(metaPath)
    if (!readResult.ok) {
      return { ok: false, error: readResult.error }
    }

    const value = normalizeRuntimeConfig(readResult.meta.runtime_config)
    return readResult.repaired ? { ok: true, repaired: true, value } : { ok: true, value }
  } catch (error: unknown) {
    return { ok: false, error: error instanceof Error ? error.message : String(error) }
  }
}

export async function setProjectRuntimeConfig(
  metaPath: string,
  config: ProjectRuntimeConfig
): Promise<ProjectRuntimeConfigWriteResult> {
  try {
    const readResult = await readProjectMetaFile(metaPath)
    if (!readResult.ok) {
      return { ok: false, error: readResult.error }
    }

    const normalized = normalizeRuntimeConfigInternal(config)
    const issues = validateRuntimeConfig(normalized)
    if (issues.length > 0) {
      return { ok: false, error: 'invalid runtime config', details: issues }
    }

    await writeProjectMetaFile(metaPath, {
      ...readResult.meta,
      runtime_config: normalizeRuntimeConfig(normalized) as unknown as Record<string, unknown>,
    })
    return readResult.repaired ? { ok: true, repaired: true } : { ok: true }
  } catch (error: unknown) {
    return { ok: false, error: error instanceof Error ? error.message : String(error) }
  }
}
