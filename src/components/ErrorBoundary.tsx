import { Component, type ErrorInfo, type ReactNode } from 'react'
import { pushLog } from './ErrorPanel'

interface Props {
  children: ReactNode
}

interface State {
  error: Error | null
}

export default class ErrorBoundary extends Component<Props, State> {
  state: State = { error: null }

  static getDerivedStateFromError(error: Error): State {
    return { error }
  }

  componentDidCatch(error: Error, info: ErrorInfo): void {
    pushLog(
      'error',
      'react',
      `${error.message}\n${info.componentStack ?? ''}`.slice(0, 4000)
    )
  }

  private handleReload = (): void => {
    window.location.reload()
  }

  private handleDismiss = (): void => {
    this.setState({ error: null })
  }

  render(): ReactNode {
    if (!this.state.error) return this.props.children
    return (
      <div className="error-boundary">
        <div className="error-boundary-card">
          <h2>界面崩溃了</h2>
          <p className="error-boundary-msg">{this.state.error.message}</p>
          <pre className="error-boundary-stack">
            {(this.state.error.stack ?? '').slice(0, 2000)}
          </pre>
          <div className="error-boundary-actions">
            <button onClick={this.handleReload}>重新加载</button>
            <button onClick={this.handleDismiss}>尝试继续</button>
          </div>
          <p className="error-boundary-hint">
            错误已记录到「📣 日志」面板。如反复出现，请截图反馈。
          </p>
        </div>
      </div>
    )
  }
}
