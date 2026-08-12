@echo off
setlocal
rem Prefer the Node runtime bundled inside the packaged Electron app
rem (ELECTRON_RUN_AS_NODE=1). On a machine where Node was never installed this is
rem the only runtime available, so host `node` is a fallback, not the primary path.
rem Path layout mirrors the macOS wrapper (bin/imcli):
rem   bin -> app.asar.unpacked -> resources -> application root.
rem Keep this file ASCII-only: cmd.exe reads it in the system ANSI codepage, and
rem BOM-less UTF-8 comments get mis-decoded into stray commands.
set "MULTI_AI_CODE_PACKAGED_ELECTRON=%~dp0..\..\..\Multi-AI Code.exe"
if not exist "%MULTI_AI_CODE_PACKAGED_ELECTRON%" goto hostnode
set "ELECTRON_RUN_AS_NODE=1"
"%MULTI_AI_CODE_PACKAGED_ELECTRON%" "%~dp0imcli.mjs" %*
exit /b %ERRORLEVEL%
:hostnode
node "%~dp0imcli.mjs" %*
exit /b %ERRORLEVEL%
