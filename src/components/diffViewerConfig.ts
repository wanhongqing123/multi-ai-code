export type DiffMode = 'working' | 'head1' | 'commit'

export const DIFF_MODE_TABS: DiffMode[] = ['working', 'head1', 'commit']

export function diffModeLabel(mode: DiffMode): string {
  switch (mode) {
    case 'working':
      return '📝 当前改动'
    case 'head1':
      return '⏱ 最近一次 commit'
    case 'commit':
      return '🎯 指定 commit'
  }
}
