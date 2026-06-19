import SkillLibraryPanel from './SkillLibraryPanel'

interface Props {
  onClose: () => void
  targetRepo: string | null
  sessionId: string | null
  sessionRunning: boolean
  /** Bump after any skill list mutation so SkillBar refreshes. */
  onSkillsChanged: () => void
}

export default function SkillStudioDialog(props: Props): JSX.Element {
  const { onClose, onSkillsChanged, targetRepo } = props

  return (
    <div className="modal-backdrop" onClick={onClose}>
      <div className="modal skill-manager-modal" onClick={(event) => event.stopPropagation()}>
        <div className="modal-head">
          <h3>🧩 Skill 管理</h3>
          <button className="modal-close" onClick={onClose}>
            ×
          </button>
        </div>

        <div className="habit-tab-body">
          <SkillLibraryPanel targetRepo={targetRepo} onChanged={onSkillsChanged} />
        </div>
      </div>
    </div>
  )
}
