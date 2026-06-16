import type { RuntimeState } from '../../electron/preload'
import { canSendRuntimeLog, getRuntimeStatusLabel } from './ProjectBuildPanel.js'

export interface RuntimeLogDialogProps {
  open: boolean
  currentProjectName: string | null
  currentProjectId: string | null
  runtimeState: RuntimeState
  sessionId: string | null
  sessionStatus: 'idle' | 'running' | 'exited'
  comment: string
  onCommentChange: (next: string) => void
  onClose: () => void
  onStopRuntime: () => void
  onSendRuntimeLog: (comment: string) => void
}

function formatRuntimeMeta(state: RuntimeState): string {
  const parts = [
    state.command ? `命令: ${state.command}` : null,
    state.cwd ? `cwd: ${state.cwd}` : null,
    state.visualStudioDisplayName ? `VS: ${state.visualStudioDisplayName}` : null,
  ].filter(Boolean)
  return parts.length > 0 ? parts.join(' · ') : '等待运行输出'
}

export default function RuntimeLogDialog(props: RuntimeLogDialogProps): JSX.Element | null {
  if (!props.open) return null

  const sendEnabled = canSendRuntimeLog(
    props.currentProjectId,
    props.runtimeState,
    props.sessionId,
    props.sessionStatus
  )
  const stopEnabled = props.runtimeState.status === 'running'
  const logText = props.runtimeState.log.trim().length > 0 ? props.runtimeState.log : '暂无运行日志输出'

  return (
    <div className="runtime-log-dialog-overlay" role="presentation" onClick={props.onClose}>
      <section
        className="runtime-log-dialog"
        role="dialog"
        aria-modal="true"
        aria-label="运行日志"
        onClick={(event) => event.stopPropagation()}
      >
        <div className="runtime-log-dialog-head">
          <div>
            <div className="build-panel-eyebrow">项目运行</div>
            <h2 className="build-panel-title">
              运行日志
              {props.currentProjectName ? (
                <span className="build-panel-project"> · {props.currentProjectName}</span>
              ) : null}
            </h2>
          </div>
          <button className="build-panel-close" onClick={props.onClose} aria-label="关闭运行日志">
            ×
          </button>
        </div>

        <div className="runtime-log-dialog-status">
          <span className={`build-panel-status build-panel-status-${props.runtimeState.status}`}>
            {getRuntimeStatusLabel(props.runtimeState.status)}
          </span>
          <span>{formatRuntimeMeta(props.runtimeState)}</span>
        </div>

        <pre className="runtime-log-dialog-log">{logText}</pre>

        <div className="runtime-log-dialog-comment">
          <label htmlFor="runtime-log-comment">补充问题</label>
          <textarea
            id="runtime-log-comment"
            value={props.comment}
            onChange={(event) => props.onCommentChange(event.target.value)}
            placeholder="例如：帮我分析下这个日志，然后解释下为什么视频没有播放出来。"
            rows={3}
          />
        </div>

        <div className="runtime-log-dialog-actions">
          <button
            className="tile-btn"
            onClick={props.onStopRuntime}
            disabled={!stopEnabled}
            title={stopEnabled ? '停止当前运行进程' : '当前没有正在运行的进程'}
          >
            停止运行
          </button>
          <button
            className="tile-btn primary"
            onClick={() => props.onSendRuntimeLog(props.comment)}
            disabled={!sendEnabled}
            title={sendEnabled ? '发送当前运行日志和补充问题' : '需要运行日志和正在运行的主会话'}
          >
            发送分析
          </button>
        </div>

        {props.runtimeState.log.trim() && (!props.sessionId || props.sessionStatus !== 'running') ? (
          <p className="build-panel-note">主会话未运行，无法发送运行日志。</p>
        ) : null}
      </section>
    </div>
  )
}
