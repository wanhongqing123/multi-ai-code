export type HabitEventKind =
  | 'pty_cmd'
  | 'ai_prompt_main'
  | 'ai_prompt_repo'
  | 'diff_annotation'
  | 'repo_view_annotation'
  | 'template_used'
  | 'plan_imported'
  | 'panel_open'
  | 'action_triggered'
  | 'site_visit'
  | 'site_click'
  | 'site_input_hint'
  | 'tab_switch'

export const ALL_HABIT_EVENT_KINDS: HabitEventKind[] = [
  'pty_cmd',
  'ai_prompt_main',
  'ai_prompt_repo',
  'diff_annotation',
  'repo_view_annotation',
  'template_used',
  'plan_imported',
  'panel_open',
  'action_triggered',
  'site_visit',
  'site_click',
  'site_input_hint',
  'tab_switch'
]

export const HABIT_KIND_LABELS: Record<HabitEventKind, string> = {
  pty_cmd: '主会话终端命令',
  ai_prompt_main: '主会话 AI prompt',
  ai_prompt_repo: '仓库查看 AI prompt',
  diff_annotation: 'Diff 批注',
  repo_view_annotation: '仓库查看代码标注',
  template_used: '模板调用',
  plan_imported: '方案导入',
  panel_open: '面板打开',
  action_triggered: '功能触发',
  site_visit: '网站访问',
  site_click: '网站点击',
  site_input_hint: '网站输入提示',
  tab_switch: '标签页切换'
}

export interface HabitSettings {
  enabled: boolean
  kinds: Partial<Record<HabitEventKind, boolean>>
  retentionDays: number
  firstRunNoticeShownAt: number
  lastAggregatedAt: number
  collectManagedChrome: boolean
  autoEnableLowRiskFlows: boolean
  autoPersonalizeUi: boolean
}

export interface HabitEventRow {
  id: number
  ts: number
  kind: string
  payload: string
  source: 'app_ui' | 'managed_chrome' | null
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

export type HabitFlowKind = 'app-flow' | 'site-flow' | 'ui-adjustment'
export type HabitFlowRisk = 'low' | 'high'
export type HabitFlowStatus = 'candidate' | 'active' | 'disabled'

export interface HabitFlowRow {
  id: number
  kind: HabitFlowKind
  title: string
  summary: string
  evidence_count: number
  risk_level: HabitFlowRisk
  enabled_by_default: number
  status: HabitFlowStatus
  payload: string
  created_at: number
  updated_at: number
}
