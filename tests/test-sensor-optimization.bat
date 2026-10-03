@echo off
setlocal
cd /d "%~dp0.."
where cl >nul 2>nul
if errorlevel 1 call "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
where cl >nul 2>nul
if errorlevel 1 exit /b 1
if not exist build mkdir build
cl /nologo /std:c++17 /utf-8 /EHsc /W4 /DUNICODE /D_UNICODE tests\sensor-optimization-test.cpp /Fo:build\sensor-optimization-test.obj /Fe:build\sensor-optimization-test.exe
if errorlevel 1 exit /b 1
build\sensor-optimization-test.exe
exit /b %errorlevel%
