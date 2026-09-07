@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0docker\run.ps1" %*
