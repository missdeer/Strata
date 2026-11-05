@echo off
setlocal
cd /d D:\Projects\Strata
call "C:\Program Files (x86)\Intel\oneAPI\2026.1\oneapi-vars.bat" > tmp\consolidated-cpu-env.log 2>&1
if errorlevel 1 exit /b 1
cmake --build build-network --target strata-host strata_network_backend strata-network-check strata-cluster-test-role strata-network-test-backend strata-server-test-engine strata-server-batch-test-engine strata-mcp-test-server strata-vision-test-encoder strata-server-check strata-text-check --parallel 6 > tmp\consolidated-cpu-build.log 2>&1
set "result=%errorlevel%"
powershell.exe -NoProfile -Command "Get-Content tmp/consolidated-cpu-build.log -Tail 45"
exit /b %result%

