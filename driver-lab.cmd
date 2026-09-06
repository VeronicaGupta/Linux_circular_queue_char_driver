@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0docker\run.ps1" %*
exit /b %ERRORLEVEL%
