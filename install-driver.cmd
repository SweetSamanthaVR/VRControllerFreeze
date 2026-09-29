@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\install-driver.ps1" %*
exit /b %ERRORLEVEL%
