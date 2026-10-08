@echo off
setlocal
cd /d D:\Projects\Strata
call "C:\Program Files (x86)\Intel\oneAPI\2026.1\oneapi-vars.bat" > "tmp\network-build-env.log" 2>&1
if errorlevel 1 exit /b 1
cmake -S . -B build-network -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=C:/Program Files (x86)/Intel/oneAPI/compiler/2026.1/bin/icx-cl.exe" "-DCMAKE_CXX_COMPILER=C:/Program Files (x86)/Intel/oneAPI/compiler/2026.1/bin/icx-cl.exe" "-DCMAKE_LINKER=%VCToolsInstallDir%bin/Hostx64/x64/link.exe" "-DCMAKE_AR=%VCToolsInstallDir%bin/Hostx64/x64/lib.exe" "-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /EHsc" "-DCMAKE_CXX_FLAGS_RELEASE=/O2 /DNDEBUG /fp:precise" "-DCMAKE_C_FLAGS_RELEASE=/O2 /DNDEBUG /fp:precise" -DSTRATA_ENABLE_CUDA=OFF -DSTRATA_ENABLE_HIP=OFF -DSTRATA_PORTABLE=ON -DSTRATA_BUILD_TESTS=OFF -DSTRATA_GGML_DIR=D:/Projects/Strata/build-intel/_deps/strata_llamacpp-src > "tmp\network-configure.log" 2>&1
if errorlevel 1 (
  type "tmp\network-configure.log"
  exit /b 1
)
cmake --build build-network --target strata_network_backend strata-network-check --parallel 6 > "tmp\network-build.log" 2>&1
set "build_result=%errorlevel%"
powershell.exe -NoProfile -Command "Get-Content -LiteralPath tmp\network-build.log -Tail 35"
exit /b %build_result%
