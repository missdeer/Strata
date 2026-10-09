@echo off
setlocal EnableExtensions DisableDelayedExpansion
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0apply-update.ps1"
if errorlevel 1 (
  echo Update did not complete. Close the old worker before retrying.
  pause
  exit /b 1
)
call "%~dp0start-profiled-host11.bat"
