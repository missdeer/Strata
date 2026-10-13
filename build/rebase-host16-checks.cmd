@echo off
setlocal
cd /d D:\Projects\Strata
call "C:\Program Files (x86)\Intel\oneAPI\2026.1\oneapi-vars.bat" > tmp\rebase-20261012\host16-env.log 2>&1
if errorlevel 1 exit /b 1
cmake --build build-intel --target strata-network-check strata-device --parallel 6 > tmp\rebase-20261012\host16-diagnostics-build.log 2>&1
if errorlevel 1 (
  type tmp\rebase-20261012\host16-diagnostics-build.log
  exit /b 1
)
python -u build\rebase-host16-checks.py
exit /b %errorlevel%
