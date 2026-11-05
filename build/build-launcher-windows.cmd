@echo off
call build\build-native-launcher.cmd
if errorlevel 1 exit /b 1
call build\build-launcher-cuda.cmd
if errorlevel 1 exit /b 1
call build\build-launcher-hip.cmd
exit /b %errorlevel%
