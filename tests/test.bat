@echo off
setlocal
cd /d "%~dp0.."
call tools\vsenv.bat
if errorlevel 1 exit /b 1
if not exist build mkdir build
copy /y IntelMSR.bin build\\IntelMSR.bin >nul
cl /nologo /std:c++17 /utf-8 /EHsc /W4 /DUNICODE /D_UNICODE tests\diagnostics.cpp /Fo:build\diagnostics.obj /Fe:build\diagnostics.exe
if errorlevel 1 exit /b 1
build\diagnostics.exe %*
exit /b %errorlevel%
