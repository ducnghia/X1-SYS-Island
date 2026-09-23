@echo off
setlocal
cd /d "%~dp0"
where cl >nul 2>nul
if errorlevel 1 (
  if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" call "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
)
where cl >nul 2>nul
if errorlevel 1 (
  echo Install Visual Studio C++ Build Tools, or use an x64 Native Tools prompt.
  exit /b 1
)
if not exist build mkdir build
set "SYS_OUTPUT=X1-SYS-Island.exe"
if not "%~1"=="" set "SYS_OUTPUT=%~1"
rc /nologo /fo build\\x1_sys_island.res x1_sys_island.rc
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /utf-8 /O1 /GL /Gy /Gw /EHsc /W4 /DUNICODE /D_UNICODE /Fo:build\x1_sys_island.obj x1_sys_island.cpp build\x1_sys_island.res /link /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO /SUBSYSTEM:WINDOWS /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'" /OUT:"%SYS_OUTPUT%"
exit /b %errorlevel%

