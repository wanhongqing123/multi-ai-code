import React from 'react'
import ReactDOM from 'react-dom/client'
import App from './App'
import './styles.css'

// Prevent Electron from navigating to a dropped file outside our drop zones
window.addEventListener('dragover', (e) => e.preventDefault())
window.addEventListener('drop', (e) => e.preventDefault())

ReactDOM.createRoot(document.getElementById('root')!).render(
  <React.StrictMode>
    <App />
  </React.StrictMode>
)
