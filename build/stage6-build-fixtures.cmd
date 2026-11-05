@echo off
setlocal
cd /d D:\Projects\Strata
call "C:\Program Files (x86)\Intel\oneAPI\2026.1\oneapi-vars.bat" > tmp\stage6-build-fixtures-env.log 2>&1
if errorlevel 1 exit /b 1
cmake -S . -B build-network -DSTRATA_BUILD_TESTS=ON > tmp\stage6-build-fixtures-configure.log 2>&1
if errorlevel 1 exit /b 1
cmake --build build-network --target strata-host strata_network_backend strata-network-check strata-cluster-test-role strata-network-test-backend strata-server-test-engine strata-server-batch-test-engine strata-mcp-test-server strata-vision-test-encoder strata-server-check strata-text-check --parallel 4 > tmp\stage6-build-fixtures.log 2>&1
exit /b %errorlevel%
