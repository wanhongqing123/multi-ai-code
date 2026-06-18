import SkillGraphPanel from './skill-graph/SkillGraphPanel'

interface Props {
  onClose: () => void
  targetRepo: string | null
  sessionId: string | null
  sessionRunning: boolean
}

export default function SkillGraphDialog(props: Props): JSX.Element {
  const { onClose, targetRepo, sessionId, sessionRunning } = props

  return (
    <div className="modal-backdrop" onClick={onClose}>
      <div className="modal skill-manager-modal" onClick={(event) => event.stopPropagation()}>
        <div className="modal-head">
          <h3>🧩 Skill 编排</h3>
          <button className="modal-close" onClick={onClose}>
            ×
          </button>
        </div>

        <div className="habit-tab-body">
          <SkillGraphPanel
            targetRepo={targetRepo}
            sessionId={sessionId}
            sessionRunning={sessionRunning}
          />
        </div>
      </div>
    </div>
  )
}
