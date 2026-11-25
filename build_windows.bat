@echo off
setlocal
cd /d "%~dp0"
if "%MINGW_BIN%"=="" set "MINGW_BIN=D:\software\mingw32\bin"
if not exist "%MINGW_BIN%\g++.exe" (
  echo [LanDrop] MinGW not found at %MINGW_BIN%
  echo Set MINGW_BIN to the folder containing g++.exe and try again.
  exit /b 1
)
set "PATH=%MINGW_BIN%;%PATH%"
mingw32-make -f Makefile.win %*
if errorlevel 1 exit /b %errorlevel%
if "%~1"=="" (
  echo.
  echo Built: build\LanDrop.exe
)
