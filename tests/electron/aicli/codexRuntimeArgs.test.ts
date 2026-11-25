import { describe, expect, it } from 'vitest'
import { withEmbeddedCodexArgs } from '../../../electron/aicli/codexRuntime.js'

describe('withEmbeddedCodexArgs', () => {
  it.each([
    { args: [] },
    { args: ['Fix the failing test'] },
    { args: ['--no-alt-screen', '--model', 'test-model'] },
    { args: ['resume', '--last'] },
    { args: ['fork', 'saved-session'] },
    { args: ['agents'] },
    { args: ['--model', 'login', 'resume', '--last'] },
    { args: ['--model=login', 'fork', '--last'] },
    { args: ['--', 'login'] },
    { args: ['--', '--remote'] },
    { args: ['--image', 'login', 'review'] }
  ])('selects the embedded app server for interactive args $args', ({ args }) => {
    const original = [...args]
    const result = withEmbeddedCodexArgs('codex', Object.freeze(args))

    expect(result).toEqual(['--no-daemon', ...original])
    expect(args).toEqual(original)
  })

  it.each([
    { args: ['--remote', 'ws://localhost:4321'] },
    { args: ['--remote=unix://'] },
    { args: ['resume', '--last', '--remote', 'ws://localhost:4321'] },
    { args: ['fork', '--remote=ws://localhost:4321', '--last'] },
    { args: ['--remote-auth-token-env', 'REMOTE_TOKEN'] },
    { args: ['--no-daemon'] },
    { args: ['resume', '--last', '--no-daemon'] },
    { args: ['--no-daemon', '--remote=unix://'] },
    { args: ['--help'] },
    { args: ['-h'] },
    { args: ['--version'] },
    { args: ['-V'] }
  ])('preserves an explicit transport or inspection request $args', ({ args }) => {
    expect(withEmbeddedCodexArgs('codex', Object.freeze(args))).toEqual(args)
  })

  it.each([
    'tcp-tunnel', 'exec', 'e', 'review', 'login', 'git-credential', 'logout',
    'mcp', 'plugin', 'app-server', 'remote-control', 'app', 'completion',
    'update', 'doctor', 'sandbox', 'debug', 'execpolicy', 'apply', 'a', 'queue',
    'archive', 'delete', 'migrate-rollouts', 'migrate-nfs-db', 'unarchive',
    'cloud', 'cloud-tasks', 'responses-api-proxy', 'exec-server', 'features', 'help'
  ])('preserves the %s subcommand and its service choices', (subcommand) => {
    const args = ['-c', 'model="test-model"', subcommand, '--fixture-arg']
    expect(withEmbeddedCodexArgs('codex', Object.freeze(args))).toEqual(args)
  })

  it.each(['--image=fixture.png', '-i=fixture.png', '-ifixture.png'])(
    'keeps the subcommand following an attached image value %s', (image) => {
      const args = [image, 'review']
      expect(withEmbeddedCodexArgs('codex', args)).toEqual(args)
    }
  )

  it.each(['--model', '-m', '--config', '-c', '--cd', '-C', '--profile', '-p',
    '--local-provider', '--sandbox', '-s', '--ask-for-approval', '-a',
    '--add-dir', '--enable', '--disable', '--multi-ai-code-im-ipc']) (
    'does not mistake the value of %s for a subcommand', (option) => {
      const args = [option, 'login']
      expect(withEmbeddedCodexArgs('codex', args)).toEqual(['--no-daemon', ...args])
    }
  )

  it.each(['codex.exe', '"C:\\bundled\\codex.exe"', '/bundle/codex']) (
    'recognizes the bundled Codex command %s', (command) => {
      expect(withEmbeddedCodexArgs(command, [])).toEqual(['--no-daemon'])
    }
  )

  it.each(['claude', 'custom-cli', 'codex-helper']) (
    'keeps arguments for %s unchanged', (command) => {
      const args = Object.freeze(['resume', '--last'])
      expect(withEmbeddedCodexArgs(command, args)).toEqual(args)
    }
  )

  it('retains sandbox and approval settings verbatim while selecting the transport', () => {
    const args = ['--sandbox', 'read-only', '--ask-for-approval', 'untrusted']
    expect(withEmbeddedCodexArgs('codex', args)).toEqual(['--no-daemon', ...args])
  })
})
