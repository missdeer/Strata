@echo off
setlocal EnableExtensions DisableDelayedExpansion
cd /d "%~dp0.."
if not exist "network.key" (
  echo Missing network.key
  pause
  exit /b 1
)
set /p STRATA_NETWORK_KEY=<"network.key"
set "STRATA_NETWORK_PROFILE=1"
echo Starting the optimized worker and preloading experts.
echo Progress and timing log: %CD%\network-opt-worker.log
echo Keep this window open. Ctrl+C stops the worker.
where python.exe >nul 2>&1
if errorlevel 1 (
  py -3 -u tools\network_worker.py --cluster-config cluster.json --node host11 >network-opt-worker.log 2>&1
) else (
  python -u tools\network_worker.py --cluster-config cluster.json --node host11 >network-opt-worker.log 2>&1
)
echo Worker exited. See network-opt-worker.log for details.
pause
