import React from 'react'
import ReactDOM from 'react-dom/client'
import App from './App'
import RepoViewerWindow from './repo-view/RepoViewerWindow'
import './styles.css'
import { pushLog } from './components/ErrorPanel'
import ErrorBoundary from './components/ErrorBoundary'
import { applyTheme } from './utils/theme.js'
import { parseRendererWindowModeSearch } from './repo-view/windowMode.js'

applyTheme()

// Prevent Electron from navigating to a dropped file outside our drop zones
window.addEventListener('dragover', (e) => e.preventDefault())
window.addEventListener('drop', (e) => e.preventDefault())

// Mirror every alert() into the error panel so users can review later
const nativeAlert = window.alert.bind(window)
window.alert = (msg?: any) => {
  const text = String(msg ?? '')
  pushLog('error', 'UI', text)
  nativeAlert(text)
}

// Capture unhandled errors / rejections into the log
window.addEventListener('error', (e) => {
  pushLog('error', 'window', `${e.message} @ ${e.filename}:${e.lineno}`)
})
window.addEventListener('unhandledrejection', (e) => {
  const reason = e.reason instanceof Error ? e.reason.message : String(e.reason)
  pushLog('error', 'promise', reason)
})

const windowMode = parseRendererWindowModeSearch(window.location.search)

ReactDOM.createRoot(document.getElementById('root')!).render(
  <React.StrictMode>
    <ErrorBoundary>
      {windowMode.kind === 'repo-view' ? (
        <RepoViewerWindow projectId={windowMode.projectId} />
      ) : (
        <App />
      )}
    </ErrorBoundary>
  </React.StrictMode>
)
