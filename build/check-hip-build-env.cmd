@echo off
setlocal EnableDelayedExpansion
set "ROCM_VENV=D:\Projects\Strata\.rocm-win"
for /f "delims=" %%R in ('"%ROCM_VENV%\Scripts\rocm-sdk.exe" path --root') do set "ROCM=%%R"
echo ROCM=[%ROCM%]
dir "%ROCM%\lib\llvm\bin\clang++.exe"
if exist "%ROCM%\lib\llvm\bin\clang++.exe" (echo COMPILER_FOUND) else (echo COMPILER_MISSING)
"%ROCM%\lib\llvm\bin\clang++.exe" --version
exit /b %errorlevel%
