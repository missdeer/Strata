@echo off
setlocal EnableExtensions DisableDelayedExpansion
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0apply-final-config.ps1"
if errorlevel 1 (
  pause
  exit /b 1
)
call "%~dp0start-profiled-host11.bat"
