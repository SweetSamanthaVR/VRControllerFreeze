@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\uninstall-driver.ps1" %*
exit /b %ERRORLEVEL%
