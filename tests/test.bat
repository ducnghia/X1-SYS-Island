@echo off
setlocal
cd /d "%~dp0.."
where cl >nul 2>nul
if errorlevel 1 call "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist build mkdir build
copy /y IntelMSR.bin build\\IntelMSR.bin >nul
cl /nologo /std:c++17 /utf-8 /EHsc /W4 /DUNICODE /D_UNICODE tests\diagnostics.cpp /Fo:build\diagnostics.obj /Fe:build\diagnostics.exe
if errorlevel 1 exit /b 1
build\diagnostics.exe %*
exit /b %errorlevel%
