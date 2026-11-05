@echo off
setlocal
cd /d D:\Projects\Strata
call "C:\Program Files (x86)\Intel\oneAPI\2026.1\oneapi-vars.bat" > "build-intel\oneapi-env.log" 2>&1
if errorlevel 1 exit /b 1
icx-cl --version
cmake -S . -B build-intel -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=C:/Program Files (x86)/Intel/oneAPI/compiler/2026.1/bin/icx-cl.exe" "-DCMAKE_CXX_COMPILER=C:/Program Files (x86)/Intel/oneAPI/compiler/2026.1/bin/icx-cl.exe" "-DCMAKE_LINKER=%VCToolsInstallDir%bin/Hostx64/x64/link.exe" "-DCMAKE_AR=%VCToolsInstallDir%bin/Hostx64/x64/lib.exe" "-DCMAKE_CXX_FLAGS_RELEASE=/O3 /DNDEBUG /QxHost /fp:precise" "-DCMAKE_C_FLAGS_RELEASE=/O3 /DNDEBUG /QxHost /fp:precise" -DSTRATA_ENABLE_CUDA=ON -DSTRATA_ENABLE_HIP=OFF -DSTRATA_ENABLE_SYCL=OFF -DSTRATA_BUILD_TESTS=OFF -DCMAKE_CUDA_ARCHITECTURES=89 -DCMAKE_CUDA_HOST_COMPILER=cl.exe -DFETCHCONTENT_UPDATES_DISCONNECTED=ON > "build-intel\oneapi-configure.log" 2>&1
if errorlevel 1 (
  type "build-intel\oneapi-configure.log"
  exit /b 1
)
type "build-intel\oneapi-configure.log"
cmake --build build-intel --target strata --parallel -- -k 99 > "build-intel\oneapi-build.log" 2>&1
set "strata_build_result=%errorlevel%"
powershell.exe -NoProfile -Command "Get-Content -LiteralPath build-intel\oneapi-build.log -Tail 25"
exit /b %strata_build_result%
