@echo off
setlocal
cd /d D:\Projects\Strata
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > tmp\rebase-builder-env.log 2>&1
if errorlevel 1 exit /b 1
cmake -S tools/cluster_build -B build-cluster/builder -G Ninja -DCMAKE_BUILD_TYPE=Release -DSTRATA_BUILDER_TESTS=ON > tmp\rebase-builder-configure.log 2>&1
if errorlevel 1 exit /b 1
cmake --build build-cluster/builder --parallel 4 > tmp\rebase-builder-build.log 2>&1
if errorlevel 1 exit /b 1
ctest --test-dir build-cluster/builder --output-on-failure > tmp\rebase-builder-test.log 2>&1
exit /b %errorlevel%
