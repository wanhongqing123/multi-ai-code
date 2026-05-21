export function stretchTerminalRootToHost(host: ParentNode): boolean {
  const root = host.querySelector<HTMLElement>('.xterm')
  if (!root) return false
  root.style.width = '100%'
  root.style.height = '100%'
  return true
}
