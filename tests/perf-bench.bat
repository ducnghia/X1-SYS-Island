@echo off
setlocal
cd /d "%~dp0.."
call tools\vsenv.bat
if errorlevel 1 exit /b 1
if not exist build mkdir build
rc /nologo /fo build\x1_sys_island.res x1_sys_island.rc
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /utf-8 /O1 /EHsc /W4 /DUNICODE /D_UNICODE tests\perf-bench.cpp build\x1_sys_island.res /Fo:build\perf-bench.obj /Fe:build\perf-bench.exe /link /SUBSYSTEM:CONSOLE
if errorlevel 1 exit /b 1
build\perf-bench.exe
exit /b %errorlevel%
