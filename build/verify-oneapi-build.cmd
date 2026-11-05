@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cmake -S . -B build-intel -DFETCHCONTENT_UPDATES_DISCONNECTED=ON
if errorlevel 1 exit /b 1
cmake --build build-intel --target strata -j 4
exit /b %errorlevel%
