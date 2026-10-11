@echo off
setlocal
for %%A in (%*) do if /i "%%~A"=="--native" goto native
python "%~dp0build_cluster.py" %*
exit /b %errorlevel%

:native
call "%~dp0build_cluster_native.bat" %*
exit /b %errorlevel%
