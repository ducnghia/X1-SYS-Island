@echo off
setlocal
cd /d "%~dp0.."
call tools\vsenv.bat
if errorlevel 1 exit /b 1
if not exist build mkdir build
cl /nologo /std:c++17 /utf-8 /EHsc /W4 /DUNICODE /D_UNICODE tests\sensor-optimization-test.cpp /Fo:build\sensor-optimization-test.obj /Fe:build\sensor-optimization-test.exe
if errorlevel 1 exit /b 1
build\sensor-optimization-test.exe
exit /b %errorlevel%
