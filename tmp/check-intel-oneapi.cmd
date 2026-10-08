@echo off
setlocal
cd /d D:\Projects\Strata
call "C:\Program Files (x86)\Intel\oneAPI\2026.1\oneapi-vars.bat" >nul 2>&1
if errorlevel 1 exit /b 1
cmake --build build-intel --target iq_avx2_parity router_dot_parity --parallel 4 > build-intel\oneapi-check-build.log 2>&1
if errorlevel 1 (
  powershell.exe -NoProfile -Command "Get-Content -LiteralPath build-intel\oneapi-check-build.log -Tail 30"
  exit /b 1
)
build-intel\iq_avx2_parity.exe > build-intel\oneapi-cpu-parity.log 2>&1
set "strata_parity_result=%errorlevel%"
powershell.exe -NoProfile -Command "Get-Content -LiteralPath build-intel\oneapi-cpu-parity.log -Tail 12"
if not "%strata_parity_result%"=="0" exit /b %strata_parity_result%
build-intel\router_dot_parity.exe > build-intel\oneapi-router-parity.log 2>&1
set "strata_router_result=%errorlevel%"
type build-intel\oneapi-router-parity.log
exit /b %strata_router_result%
