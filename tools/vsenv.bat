@echo off
rem Puts an x64 MSVC toolchain on PATH for the calling script.
rem Uses vswhere so any Visual Studio 2022 edition works, not only Build Tools.
rem Intentionally no setlocal: the caller must inherit the environment.
where cl >nul 2>nul
if not errorlevel 1 exit /b 0
set "X1_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "X1_VSROOT="
if exist "%X1_VSWHERE%" (
  for /f "usebackq delims=" %%i in (`"%X1_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "X1_VSROOT=%%i"
)
if not defined X1_VSROOT set "X1_VSROOT=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools"
if exist "%X1_VSROOT%\VC\Auxiliary\Build\vcvars64.bat" call "%X1_VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
set "X1_VSWHERE="
set "X1_VSROOT="
where cl >nul 2>nul
if errorlevel 1 (
  echo Install Visual Studio 2022 C++ Build Tools, or use an x64 Native Tools prompt.
  exit /b 1
)
exit /b 0
