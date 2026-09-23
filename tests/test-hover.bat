@echo off
setlocal
cd /d "%~dp0.."
where cl >nul 2>nul
if errorlevel 1 call "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist build mkdir build
rc /nologo /fo build\x1_sys_island.res x1_sys_island.rc
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /utf-8 /W4 /EHsc /DUNICODE /D_UNICODE tests\hover-test.cpp build\x1_sys_island.res /Fo:build\hover-test.obj /Fe:build\hover-test.exe /link /SUBSYSTEM:CONSOLE
if errorlevel 1 exit /b 1
build\hover-test.exe
exit /b %errorlevel%
