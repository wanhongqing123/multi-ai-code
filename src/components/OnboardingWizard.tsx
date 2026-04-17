import { useState } from 'react'

export interface OnboardingWizardProps {
  onClose: () => void
  onDone: (params: { projectId: string; planName: string }) => void
}

type Step = 1 | 2 | 3

export default function OnboardingWizard({ onClose, onDone }: OnboardingWizardProps) {
  const [step, setStep] = useState<Step>(1)
  const [repoPath, setRepoPath] = useState('')
  const [projectName, setProjectName] = useState('')
  const [planName, setPlanName] = useState('')
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState<string | null>(null)

  async function pickRepo() {
    const res = await window.api.project.pickDir()
    if (res.canceled || !res.path) return
    setRepoPath(res.path)
    if (!projectName) {
      const base = res.path.split(/[/\\]/).filter(Boolean).pop() ?? ''
      setProjectName(base)
    }
  }

  async function finish() {
    if (!repoPath) {
      setError('请先选择代码仓库目录')
      setStep(1)
      return
    }
    if (!projectName.trim()) {
      setError('请输入项目名称')
      setStep(2)
      return
    }
    if (!planName.trim()) {
      setError('请输入方案名称')
      setStep(3)
      return
    }
    setBusy(true)
    const res = await window.api.project.create(projectName.trim(), repoPath)
    setBusy(false)
    if (!res.ok || !res.id) {
      setError(`创建项目失败：${res.error ?? ''}`)
      return
    }
    localStorage.setItem('multi-ai-code.onboarding-done', '1')
    onDone({ projectId: res.id, planName: planName.trim() })
  }

  return (
    <div className="modal-backdrop">
      <div className="modal onboarding-modal" onClick={(e) => e.stopPropagation()}>
        <div className="modal-head">
          <h3>🚀 欢迎使用 Multi-AI Code</h3>
          <button className="modal-close" onClick={onClose}>×</button>
        </div>

        <div className="onboarding-steps">
          <div className={`onb-step ${step === 1 ? 'active' : step > 1 ? 'done' : ''}`}>
            <span className="onb-num">1</span>选仓库
          </div>
          <div className="onb-sep" />
          <div className={`onb-step ${step === 2 ? 'active' : step > 2 ? 'done' : ''}`}>
            <span className="onb-num">2</span>命名项目
          </div>
          <div className="onb-sep" />
          <div className={`onb-step ${step === 3 ? 'active' : ''}`}>
            <span className="onb-num">3</span>定义方案
          </div>
        </div>

        <div className="onboarding-body">
          {step === 1 && (
            <>
              <h4>第 1 步：选择代码仓库目录</h4>
              <p>
                Multi-AI Code 会围绕一个真实的代码仓库运作。Stage 2-4 的 CLI 会直接把这个目录当作
                工作目录（通过符号链接挂载），AI 对代码的修改会真实写入这里。
              </p>
              <button className="drawer-btn primary" onClick={pickRepo}>
                📂 选择目录…
              </button>
              {repoPath && (
                <div className="onb-hint">
                  已选：<code>{repoPath}</code>
                </div>
              )}
            </>
          )}
          {step === 2 && (
            <>
              <h4>第 2 步：给项目起个名字</h4>
              <p>
                这个名字会显示在顶栏和项目切换列表里，用来区分多个项目。建议用代码库名字（如
                "u3player"）。
              </p>
              <input
                className="plan-name-input"
                value={projectName}
                onChange={(e) => setProjectName(e.target.value)}
                placeholder="如：u3player / 我的后端服务 / …"
                autoFocus
              />
            </>
          )}
          {step === 3 && (
            <>
              <h4>第 3 步：输入本次要做的方案名称</h4>
              <p>
                方案名会作为本次四阶段产物的归档标题（例如"用户登录流程优化 v2"）。
                后面所有版本都会追加到同一文件里，方便回溯。
              </p>
              <input
                className="plan-name-input"
                value={planName}
                onChange={(e) => setPlanName(e.target.value)}
                placeholder="如：用户登录流程优化 v2 / 修复 VFR 索引并发 / …"
                autoFocus
              />
              <div className="onb-hint" style={{ marginTop: 12 }}>
                <strong>下一步会发生什么</strong>：
                <ul>
                  <li>平台创建项目目录并写入 SQLite 元数据</li>
                  <li>Stage 1（方案设计 · codex）自动启动</li>
                  <li>你在 Stage 1 对话，产出 design.md 后点「✓ 完成 → Stage 2」推进流程</li>
                </ul>
              </div>
            </>
          )}
          {error && <div className="drawer-error" style={{ marginTop: 10 }}>⚠ {error}</div>}
        </div>

        <div className="drawer-actions">
          {step > 1 && (
            <button className="drawer-btn" onClick={() => setStep((s) => (s - 1) as Step)}>
              上一步
            </button>
          )}
          <span style={{ flex: 1 }} />
          <button className="drawer-btn" onClick={onClose}>
            稍后再说
          </button>
          {step < 3 ? (
            <button
              className="drawer-btn primary"
              onClick={() => {
                setError(null)
                if (step === 1 && !repoPath) {
                  setError('请先选择代码仓库目录')
                  return
                }
                if (step === 2 && !projectName.trim()) {
                  setError('请输入项目名称')
                  return
                }
                setStep((s) => (s + 1) as Step)
              }}
            >
              下一步 →
            </button>
          ) : (
            <button className="drawer-btn primary" onClick={finish} disabled={busy}>
              {busy ? '创建中…' : '✓ 创建并进入 Stage 1'}
            </button>
          )}
        </div>
      </div>
    </div>
  )
}
