@echo off
rem Builds CameraShake.ofx.bundle (64-bit) with MSVC, then runs the tests.
rem Usage:  build.bat [clean]
setlocal enabledelayedexpansion
set ROOT=%~dp0
set OBJ=%ROOT%build\obj
rem Tests build into their own object dir - the link step globs %OBJ%\*.obj and
rem would otherwise pick up the tests' main() on an incremental build.
set TOBJ=%ROOT%build\obj-test
set BIN=%ROOT%build\CameraShake.ofx.bundle\Contents\Win64
set RES=%ROOT%build\CameraShake.ofx.bundle\Contents\Resources

if /i "%~1"=="clean" if exist "%ROOT%build" rmdir /s /q "%ROOT%build"

if not exist "%OBJ%" mkdir "%OBJ%"
if not exist "%TOBJ%" mkdir "%TOBJ%"
if not exist "%BIN%" mkdir "%BIN%"
if not exist "%RES%" mkdir "%RES%"

rem --- locate the MSVC toolchain -------------------------------------------
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" (
  echo ERROR: vswhere.exe not found - is Visual Studio installed?
  exit /b 1
)
set VSPATH=
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSPATH=%%i
if not defined VSPATH (
  echo ERROR: no Visual Studio instance with the MSVC C++ toolset.
  echo Install the "Desktop development with C++" workload.
  exit /b 1
)
set VCVARS=%VSPATH%\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (
  echo ERROR: vcvars64.bat not found at "%VCVARS%"
  exit /b 1
)
echo Using MSVC from: %VSPATH%

call "%VCVARS%" >nul
if errorlevel 1 exit /b 1

set INCS=/I"%ROOT%vendor\openfx\include" /I"%ROOT%vendor\openfx\Support\include"
rem /MT links the CRT statically so the .ofx needs no VC++ redistributable.
set FLAGS=/nologo /EHsc /O2 /MT /std:c++17 /W3 /DWIN32 /D_WINDOWS /DNDEBUG /D_CRT_SECURE_NO_WARNINGS

rem --- plug-in --------------------------------------------------------------
cl %FLAGS% /c %INCS% /I"%ROOT%src" /Fo"%OBJ%\\" "%ROOT%src\CameraShake.cpp" "%ROOT%vendor\openfx\Support\Library\*.cpp"
if errorlevel 1 exit /b 1

link /nologo /DLL /OPT:REF /OPT:ICF /OUT:"%BIN%\CameraShake.ofx" "%OBJ%\*.obj"
if errorlevel 1 exit /b 1

del /q "%BIN%\CameraShake.lib" "%BIN%\CameraShake.exp" 2>nul

rem --- tests ----------------------------------------------------------------
rem core_test exercises the shake maths and sampling without a host.
cl %FLAGS% /I"%ROOT%src" /Fo"%TOBJ%\\" /Fe"%ROOT%build\core_test.exe" "%ROOT%test\core_test.cpp"
if errorlevel 1 exit /b 1
"%ROOT%build\core_test.exe"
if errorlevel 1 exit /b 1

rem smoke test loads the built plug-in and validates its descriptor.
cl %FLAGS% %INCS% /Fo"%TOBJ%\\" /Fe"%ROOT%build\smoke.exe" "%ROOT%test\smoke.cpp"
if errorlevel 1 exit /b 1
"%ROOT%build\smoke.exe" "%BIN%\CameraShake.ofx"
if errorlevel 1 exit /b 1

echo.
echo Built %BIN%\CameraShake.ofx
