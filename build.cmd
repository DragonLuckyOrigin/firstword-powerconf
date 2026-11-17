@echo off
rem Builds FirstWord and the unit tests with MSVC (no third-party code).
rem Usage: build.cmd [tests]   - "tests" builds and runs only the unit tests.
setlocal
cd /d "%~dp0"
if defined VCINSTALLDIR goto build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "DEVCMD="
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find Common7\Tools\VsDevCmd.bat`) do set "DEVCMD=%%i"
if not defined DEVCMD set "DEVCMD=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
if not exist "%DEVCMD%" (echo VsDevCmd.bat not found & exit /b 9)
call "%DEVCMD%" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%

:build
if not exist out mkdir out
set "FLAGS=/nologo /std:c++17 /EHsc /W4 /WX /MT /utf-8 /permissive- /DUNICODE /D_UNICODE"
cl %FLAGS% tests\unit_tests.cpp /Fo:out\ /Fe:out\unit_tests.exe
if errorlevel 1 exit /b %errorlevel%
out\unit_tests.exe
if errorlevel 1 exit /b %errorlevel%
if /i "%~1"=="tests" exit /b 0
cl %FLAGS% src\firstword.cpp /Fo:out\ /Fe:out\FirstWord.exe /link ole32.lib cfgmgr32.lib user32.lib shell32.lib /SUBSYSTEM:WINDOWS /DYNAMICBASE /NXCOMPAT
exit /b %errorlevel%
