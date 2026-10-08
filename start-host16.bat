@echo off
setlocal EnableExtensions DisableDelayedExpansion
title Strata IQ3_S - host16 + host11 OpenAI API
cd /d "%~dp0"

if exist ".venv\Scripts\python.exe" (
  ".venv\Scripts\python.exe" -u tools\start_host16.py %*
) else (
  where python.exe >nul 2>&1
  if errorlevel 1 (
    py -3 -u tools\start_host16.py %*
  ) else (
    python -u tools\start_host16.py %*
  )
)
set "HOST16_START_RESULT=%errorlevel%"
if /i "%~1"=="--check" exit /b %HOST16_START_RESULT%
echo.
pause
exit /b %HOST16_START_RESULT%
