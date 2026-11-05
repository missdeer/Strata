@echo off
setlocal
cd /d D:\Projects\Strata
call "C:\Program Files (x86)\Intel\oneAPI\2026.1\oneapi-vars.bat" > tmp\console-build-env.log 2>&1
if errorlevel 1 exit /b 1
cmake --build build-network --target strata-host strata-server-test-engine --parallel 4 > tmp\console-build-cpu.log 2>&1
if errorlevel 1 exit /b 1
cmake --build build-intel --target strata-host --parallel 4 > tmp\console-build-cuda.log 2>&1
if errorlevel 1 exit /b 1
cmake --build build-cluster/host17-core --target strata-host --parallel 4 > tmp\console-build-hip.log 2>&1
if errorlevel 1 exit /b 1
cmake --build build-sycl-text --target strata-host --parallel 4 > tmp\console-build-sycl.log 2>&1
exit /b %errorlevel%
