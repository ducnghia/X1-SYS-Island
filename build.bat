@echo off
setlocal
cd /d "%~dp0"
call tools\vsenv.bat
if errorlevel 1 exit /b 1
if not exist build mkdir build
set "SYS_OUTPUT=X1-SYS-Island.exe"
if not "%~1"=="" set "SYS_OUTPUT=%~1"
rc /nologo /fo build\\x1_sys_island.res x1_sys_island.rc
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /utf-8 /O1 /GL /Gy /Gw /EHsc /W4 /DUNICODE /D_UNICODE /Fo:build\x1_sys_island.obj x1_sys_island.cpp build\x1_sys_island.res /link /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO /SUBSYSTEM:WINDOWS /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'" /OUT:"%SYS_OUTPUT%"
exit /b %errorlevel%
