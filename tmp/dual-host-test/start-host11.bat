@echo off
setlocal
cd /d "%~dp0"
if not exist "network.key" (echo Missing network.key & pause & exit /b 1)
set /p STRATA_NETWORK_KEY=<"network.key"
where python.exe >nul 2>&1
if errorlevel 1 (
  py -3 tools\network_worker.py --cluster-config cluster.json --node host11
) else (
  python tools\network_worker.py --cluster-config cluster.json --node host11
)
echo.
echo Worker exited. Keep this window open while the worker is running.
pause
