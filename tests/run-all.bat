@echo off
rem Runs every non-interactive test. Extra arguments (for example
rem --require-temperature from an elevated prompt) are passed to test.bat.
setlocal
cd /d "%~dp0"
call test-sensor-optimization.bat
if errorlevel 1 (echo FAILED: test-sensor-optimization.bat & exit /b 1)
call test-hover.bat
if errorlevel 1 (echo FAILED: test-hover.bat & exit /b 1)
call test.bat %*
if errorlevel 1 (echo FAILED: test.bat & exit /b 1)
echo PASS: all tests
exit /b 0
