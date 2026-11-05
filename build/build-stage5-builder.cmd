@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 exit /b 1
cmake -S tools/cluster_build -B build-cluster/builder -G Ninja -DCMAKE_BUILD_TYPE=Release -DSTRATA_BUILDER_TESTS=ON > tmp/stage5-builder-configure.log 2>&1
if errorlevel 1 (type tmp\stage5-builder-configure.log & exit /b 1)
cmake --build build-cluster/builder --parallel 6 > tmp/stage5-builder-build.log 2>&1
if errorlevel 1 (type tmp\stage5-builder-build.log & exit /b 1)
ctest --test-dir build-cluster/builder --output-on-failure
