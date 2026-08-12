# PowerShell resolves a bare `imcli` to this .ps1 before imcli.cmd, which bypasses
# cmd.exe entirely. The batch wrapper cannot carry newlines inside argv (cmd stops
# parsing at the first newline, silently truncating multi-line text to its first
# line); PowerShell -> node keeps quoted multi-line arguments intact.
#
# Keep this file ASCII-only: Windows PowerShell 5.1 reads BOM-less files in the
# system ANSI codepage, and a mis-decoded byte can land on a backtick and swallow
# the following lines.
#
# Three details below were each found the hard way; removing any one fails silently:
#
# 1. Packaged runtime first. The Electron app bundles Node (ELECTRON_RUN_AS_NODE=1).
#    On a machine without Node installed it is the only runtime there is, so host
#    `node` is the fallback. Mirrors imcli.cmd and the macOS bin/imcli wrapper.
# 2. `$input |`. PowerShell binds pipeline input to the script's own $input and does
#    NOT forward it to child processes. Without this, `imcli send <user> -` (stdin
#    mode) always reads an empty stdin and prints the usage error.
# 3. Trailing `| Write-Output`. The packaged Electron is a GUI-subsystem binary, so
#    PowerShell does not wait for it: called directly, output capture returns empty,
#    $LASTEXITCODE is unreliable, and text surfaces late under a later command.
#    Attaching a pipeline forces synchronization (verified: stdin, argv, captured
#    output and exit code all correct).
$scriptPath = Join-Path $PSScriptRoot 'imcli.mjs'
$packagedElectron = Join-Path $PSScriptRoot '..\..\..\Multi-AI Code.exe'

if (Test-Path -LiteralPath $packagedElectron) {
    $hadRunAsNode = Test-Path Env:\ELECTRON_RUN_AS_NODE
    $previousRunAsNode = if ($hadRunAsNode) { $env:ELECTRON_RUN_AS_NODE } else { $null }
    $env:ELECTRON_RUN_AS_NODE = '1'
    try {
        $input | & $packagedElectron $scriptPath @args | Write-Output
    } finally {
        if ($hadRunAsNode) {
            $env:ELECTRON_RUN_AS_NODE = $previousRunAsNode
        } else {
            Remove-Item Env:\ELECTRON_RUN_AS_NODE -ErrorAction SilentlyContinue
        }
    }
} else {
    $input | & node $scriptPath @args | Write-Output
}

exit $LASTEXITCODE
