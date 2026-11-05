@echo off
setlocal EnableExtensions DisableDelayedExpansion
title Strata IQ3_S - host16 + host11 OpenAI API
cd /d "%~dp0"
set "HOST16_ENGINE=%~dp0dist\cluster\host16\strata.exe"
set "HOST16_NO_PAUSE="
for %%A in (%*) do (
  if /i "%%~A"=="--check" set "HOST16_NO_PAUSE=1"
  if /i "%%~A"=="--help" set "HOST16_NO_PAUSE=1"
  if /i "%%~A"=="-h" set "HOST16_NO_PAUSE=1"
)

if exist ".venv\Scripts\python.exe" (
  ".venv\Scripts\python.exe" -X utf8 -u tools\start_host16.py --no-api-key --prefill auto:8192 --exe "%HOST16_ENGINE%" %*
) else (
  where python.exe >nul 2>&1
  if errorlevel 1 (
    py -3 -X utf8 -u tools\start_host16.py --no-api-key --prefill auto:8192 --exe "%HOST16_ENGINE%" %*
  ) else (
    python -X utf8 -u tools\start_host16.py --no-api-key --prefill auto:8192 --exe "%HOST16_ENGINE%" %*
  )
)
set "HOST16_START_RESULT=%errorlevel%"
if defined HOST16_NO_PAUSE exit /b %HOST16_START_RESULT%
echo.
pause
exit /b %HOST16_START_RESULT%
