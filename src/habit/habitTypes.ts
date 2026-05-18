export type HabitEventKind =
  | 'pty_cmd'
  | 'ai_prompt_main'
  | 'ai_prompt_repo'
  | 'diff_annotation'
  | 'repo_view_annotation'
  | 'template_used'
  | 'plan_imported'

export const ALL_HABIT_EVENT_KINDS: HabitEventKind[] = [
  'pty_cmd',
  'ai_prompt_main',
  'ai_prompt_repo',
  'diff_annotation',
  'repo_view_annotation',
  'template_used',
  'plan_imported'
]

export const HABIT_KIND_LABELS: Record<HabitEventKind, string> = {
  pty_cmd: '主会话终端命令',
  ai_prompt_main: '主会话 AI prompt',
  ai_prompt_repo: '仓库查看 AI prompt',
  diff_annotation: 'Diff 批注',
  repo_view_annotation: '仓库查看代码标注',
  template_used: '模板调用',
  plan_imported: '方案导入'
}

export interface HabitSettings {
  enabled: boolean
  kinds: Partial<Record<HabitEventKind, boolean>>
  retentionDays: number
  firstRunNoticeShownAt: number
  lastAggregatedAt: number
}

export interface HabitEventRow {
  id: number
  ts: number
  kind: string
  payload: string
  project_id: string | null
  repo_path: string | null
  source_window: string | null
}

export const ALLOWED_RETENTION_DAYS: readonly number[] = [30, 60, 90, 180]

export type SkillCandidateStatus =
  | 'pending'
  | 'accepted'
  | 'edited'
  | 'discarded'
  | 'snoozed'
  | 'error'

export interface SkillCandidateRow {
  id: number
  created_at: number
  cluster_kind: string
  cluster_size: number
  source_event_ids: string
  representative_samples: string
  generated_title: string | null
  generated_body: string | null
  generated_meta: string | null
  status: string
  reviewed_at: number | null
  snoozed_until: number | null
  error_message: string | null
}
