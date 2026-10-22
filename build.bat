@echo off
rem Build market_fix.dll with MinGW-w64.
rem Set MINGW_BIN if g++.exe is not on PATH.

setlocal
if "%MINGW_BIN%"=="" set MINGW_BIN=
set GXX=%MINGW_BIN%g++.exe

if not exist "build" mkdir build

"%GXX%" -std=c++17 -O2 -shared -static -static-libgcc -static-libstdc++ ^
    -o build\market_fix.dll src\market_fix.cpp -lkernel32

if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)
echo Built: build\market_fix.dll
