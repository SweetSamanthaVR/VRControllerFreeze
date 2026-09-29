@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\collect-diagnostics.ps1" %*
exit /b %ERRORLEVEL%
