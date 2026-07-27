# PowerShell resolves a bare `imcli` to this .ps1 before imcli.cmd, which bypasses
# cmd.exe entirely. The batch wrapper cannot carry newlines inside argv (cmd stops
# parsing at the first newline, silently truncating multi-line text to its first
# line); PowerShell -> node keeps quoted multi-line arguments intact.
& node "$PSScriptRoot\imcli.mjs" @args
exit $LASTEXITCODE
