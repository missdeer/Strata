@echo off
setlocal
cd /d D:\Projects\Strata
call "C:\Program Files (x86)\Intel\oneAPI\2026.1\oneapi-vars.bat" > "tmp\network-cuda-env.log" 2>&1
if errorlevel 1 exit /b 1
cmake --build build-intel --target strata strata_network_backend strata-network-check --parallel 6 > "tmp\network-cuda-build.log" 2>&1
set "build_result=%errorlevel%"
powershell.exe -NoProfile -Command "Get-Content -LiteralPath tmp\network-cuda-build.log -Tail 25"
exit /b %build_result%
