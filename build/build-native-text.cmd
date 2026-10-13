@echo off
setlocal
cd /d D:\Projects\Strata
call "C:\Program Files (x86)\Intel\oneAPI\2026.1\oneapi-vars.bat" > tmp\native-text-env.log 2>&1
if errorlevel 1 exit /b 1
cmake -S . -B build-network -DSTRATA_BUILD_NATIVE_CLUSTER=ON -DSTRATA_JSONCONS_DIR=D:/Projects/Strata/tmp/jsoncons-source/jsoncons-1.7.0 > tmp\native-text-configure.log 2>&1
if errorlevel 1 (
  type tmp\native-text-configure.log
  exit /b 1
)
cmake --build build-network --target strata-text-check --parallel 6 > tmp\native-text-build.log 2>&1
set "result=%errorlevel%"
powershell.exe -NoProfile -Command "Get-Content tmp/native-text-build.log -Tail 45"
exit /b %result%
