@echo off
setlocal
cd /d D:\Projects\Strata
set "HIPVERIFY_ROCM="
for /f "delims=" %%R in ('.rocm-win\Scripts\rocm-sdk.exe path --root') do set "HIPVERIFY_ROCM=%%R"
if not defined HIPVERIFY_ROCM exit /b 1
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "HIP_PLATFORM=amd"
set "HIP_PATH=%HIPVERIFY_ROCM%"
set "ROCM_PATH=%HIPVERIFY_ROCM%"
set "PATH=%HIPVERIFY_ROCM%\bin;%HIPVERIFY_ROCM%\lib\llvm\bin;%PATH%"
"%HIPVERIFY_ROCM%\lib\llvm\bin\clang++.exe" --version
if errorlevel 1 exit /b 1
"%HIPVERIFY_ROCM%\lib\llvm\bin\clang++.exe" -std=c++20 -O3 -x hip --offload-arch=gfx1102 --offload-arch=gfx1103 "--rocm-path=%HIPVERIFY_ROCM%" "--rocm-device-lib-path=%HIPVERIFY_ROCM%\lib\llvm\amdgcn\bitcode" build\verify-hip-sdk.hip -o tmp\verify-hip-sdk.exe
if errorlevel 1 exit /b 1
"%HIPVERIFY_ROCM%\lib\llvm\bin\llvm-objdump.exe" -p tmp\verify-hip-sdk.exe | findstr /c:"DLL Name"
exit /b %errorlevel%
