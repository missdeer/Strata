@echo off
setlocal
cd /d D:\Projects\Strata
call "C:\Program Files (x86)\Intel\oneAPI\2026.1\oneapi-vars.bat" > "tmp\native-test-env.log" 2>&1
if errorlevel 1 exit /b 1
cmake -S . -B build-network -DSTRATA_BUILD_NATIVE_CLUSTER=ON -DSTRATA_BUILD_TESTS=ON > "tmp\native-test-configure.log" 2>&1
if errorlevel 1 (
  type "tmp\native-test-configure.log"
  exit /b 1
)
cmake --build build-network --target strata-network-test-backend strata-network-worker --parallel 4
exit /b %errorlevel%
