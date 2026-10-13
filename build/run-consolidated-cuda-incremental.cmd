@echo off
setlocal
cd /d D:\Projects\Strata
call "C:\Program Files (x86)\Intel\oneAPI\2026.1\oneapi-vars.bat" > tmp\consolidated-cuda-cuda-env.log 2>&1
if errorlevel 1 exit /b 1
cmake --build build-intel --target strata-host strata strata_network_backend --parallel 6 > tmp\consolidated-cuda-cuda-build.log 2>&1
set "result=%errorlevel%"
powershell.exe -NoProfile -Command "Get-Content tmp/consolidated-cuda-cuda-build.log -Tail 30"
exit /b %result%

