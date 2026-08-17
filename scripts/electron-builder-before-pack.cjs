const { existsSync } = require('node:fs')
const { join } = require('node:path')
const { spawnSync } = require('node:child_process')

function run(command, args, options = {}) {
  const result = spawnSync(command, args, {
    cwd: options.cwd,
    env: options.env,
    stdio: 'inherit'
  })
  if (result.error) throw result.error
  if (result.status !== 0) {
    throw new Error(`${command} failed with exit code ${result.status}`)
  }
}

module.exports = async function beforePack(context) {
  if (context.electronPlatformName !== 'darwin') return

  const repoRoot = join(__dirname, '..')
  const rebuild = join(repoRoot, 'node_modules', '.bin', 'electron-rebuild')
  if (!existsSync(rebuild)) {
    throw new Error(`[beforePack] electron-rebuild not found: ${rebuild}`)
  }

  const python = '/usr/bin/python3'
  const pythonCheck = spawnSync(python, ['-c', 'import distutils'])
  if (pythonCheck.status !== 0) {
    throw new Error(
      '[beforePack] /usr/bin/python3 with distutils is required to rebuild Electron native modules'
    )
  }

  const electronVersion = require(join(repoRoot, 'node_modules', 'electron', 'package.json')).version
  console.log(
    `[beforePack] force rebuilding better-sqlite3 and node-pty for Electron ${electronVersion}`
  )
  run(
    rebuild,
    [
      '-f',
      '-o',
      'better-sqlite3,node-pty',
      '-v',
      electronVersion,
      '--build-from-source'
    ],
    {
      cwd: repoRoot,
      env: {
        ...process.env,
        PYTHON: python,
        npm_config_python: python
      }
    }
  )
}
