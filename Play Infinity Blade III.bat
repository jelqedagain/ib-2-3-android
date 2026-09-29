@echo off
rem Opens the Vibefinity Blade 3 launcher (settings, key bindings, Play) for this development folder.
rem The latest build is copied to "Vibefinity Blade 3.exe" here, next to game\ and userdata\,
rem unless the game is running (a running exe cannot be replaced).
setlocal
cd /d "%~dp0"
if not exist build\ib3rt.exe (
  echo build\ib3rt.exe not found - build it first with build.sh
  pause
  exit /b 1
)

tasklist /FI "IMAGENAME eq Vibefinity Blade 3.exe" 2>nul | find /I "Vibefinity Blade 3.exe" >nul
if errorlevel 1 (
  copy /Y build\ib3rt.exe "Vibefinity Blade 3.exe" >nul
  copy /Y build\libEGL.dll . >nul
  copy /Y build\libGLESv2.dll . >nul
)

rem Saves live in userdata\ (first run: bring over saves from the old location).
if not exist userdata if exist build\userdata xcopy /E /I /Q build\userdata userdata >nul

start "" "Vibefinity Blade 3.exe"
