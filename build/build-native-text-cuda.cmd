@echo off
setlocal
cd /d D:\Projects\Strata
call "C:\Program Files (x86)\Intel\oneAPI\2026.1\oneapi-vars.bat" > tmp\native-text-cuda-env.log 2>&1
if errorlevel 1 exit /b 1
cmake -S . -B build-intel -DSTRATA_BUILD_NATIVE_CLUSTER=ON -DSTRATA_JSONCONS_DIR=D:/Projects/Strata/tmp/jsoncons-source/jsoncons-1.7.0 > tmp\native-text-cuda-configure.log 2>&1
if errorlevel 1 (
  type tmp\native-text-cuda-configure.log
  exit /b 1
)
cmake --build build-intel --target strata-text-check strata-cluster --parallel 6 > tmp\native-text-cuda-build.log 2>&1
set "result=%errorlevel%"
powershell.exe -NoProfile -Command "Get-Content tmp/native-text-cuda-build.log -Tail 30"
exit /b %result%
