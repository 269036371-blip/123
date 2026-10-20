@echo off
setlocal EnableDelayedExpansion
chcp 936 >nul 2>&1
title KeyMacro Installer - by ö°Ñôö«ÔÂ (ayjy)
cd /d "%~dp0"

set "ROOT=%~dp0"
set "BIN=%ROOT%bin\KeyMacro.exe"
set "SRC=%ROOT%src\main.cpp"
set "OUT=%ROOT%KeyMacro.exe"
set "PS1=%ROOT%pick_keys.ps1"

call :bootanim

echo.
echo  ============================================================
echo    KeyMacro  -  Windows keyboard auto-repeat macro
echo    Author: ö°Ñôö«ÔÂ (ayjy)
echo  ============================================================
echo.

rem ---------- [1/5] administrator ----------
echo  [1/5] checking administrator privileges...
net session >nul 2>&1
if !errorlevel! neq 0 (
    echo       requesting elevation, please accept the UAC prompt...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs" >nul 2>&1
    exit /b
)
echo       OK
echo.

rem ---------- [2/5] keys ----------
echo  [2/5] choose the keys to repeat
echo       default:  1 2 3 4 5 C E R G J K
set "KEYS=1 2 3 4 5 C E R G J K"
set "IN="
set /p "IN=      Enter = default, or type your own (space separated): "
if defined IN set "KEYS=!IN!"
echo       using: !KEYS!
echo.

rem ---------- [3/5] obtain exe ----------
echo  [3/5] getting KeyMacro.exe...
set "EXE="

if exist "!BIN!" (
    set "EXE=!BIN!"
    echo       bundled binary found - no download, no compile needed
)

if not defined EXE (
    set "GXX="
    for %%P in (
        "C:\msys64\ucrt64\bin\g++.exe"
        "C:\msys64\mingw64\bin\g++.exe"
        "C:\msys32\mingw32\bin\g++.exe"
        "C:\mingw64\bin\g++.exe"
        "C:\mingw32\bin\g++.exe"
    ) do (
        if not defined GXX if exist "%%~P" set "GXX=%%~P"
    )
    if not defined GXX (
        where g++ >nul 2>&1
        if !errorlevel! equ 0 set "GXX=g++"
    )
    if defined GXX (
        echo       compiling with: !GXX!
        "!GXX!" -std=c++17 -O2 -municode -mwindows "!SRC!" -o "!OUT!" -luser32 -lwinmm -lshell32 -static
        if exist "!OUT!" (
            set "EXE=!OUT!"
            echo       compiled OK
        ) else (
            echo       [X] compile failed
        )
    )
)

if not defined EXE (
    echo.
    echo       [X] no bundled binary and no C++ compiler on this PC.
    echo.
    echo       Easiest fix - download MSYS2 from a China mirror:
    echo         https://mirrors.tuna.tsinghua.edu.cn/msys2/distrib/x86_64/
    echo         https://mirrors.ustc.edu.cn/msys2/distrib/x86_64/
    echo         https://mirrors.aliyun.com/msys2/distrib/x86_64/
    echo.
    echo       Download msys2-x86_64-XXXXXXXX.exe, put it into:
    echo         !ROOT!installers
    echo       then run this installer again.
    echo.
    pause
    exit /b 1
)
echo.

rem ---------- [4/5] deploy ----------
echo  [4/5] installing...

set "DESK=%USERPROFILE%\Desktop"
if not exist "!DESK!" set "DESK=%USERPROFILE%\OneDrive\Desktop"
if not exist "!DESK!" set "DESK=%PUBLIC%\Desktop"

if not exist "!DESK!" (
    echo       [X] desktop folder not found
    pause
    exit /b 1
)

copy /Y "!EXE!" "!DESK!\KeyMacro.exe" >nul 2>&1
if exist "!DESK!\KeyMacro.exe" (
    echo       desktop: !DESK!\KeyMacro.exe
) else (
    echo       [X] copy to desktop failed
)

if exist "!PS1!" (
    powershell -NoProfile -ExecutionPolicy Bypass -File "!PS1!" -Keys "!KEYS!" >nul 2>&1
)
if exist "%APPDATA%\KeyMacro\KeyMacro.ini" (
    echo       config:  %APPDATA%\KeyMacro\KeyMacro.ini
) else (
    echo       [i] config will be created on first run
)
echo.

rem ---------- [5/5] autostart ----------
echo  [5/5] finish
set "AUTO="
set /p "AUTO=      start with Windows? (Y/N, Enter = N): "
if /i "!AUTO!"=="Y" (
    copy /Y "!EXE!" "%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\KeyMacro.exe" >nul 2>&1
    echo       autostart enabled
) else (
    echo       autostart skipped
)

echo.
echo  ============================================================
echo    Done.  KeyMacro.exe is on your desktop.
echo.
echo    F8              toggle on / off
echo    Ctrl+Alt+F12    quit
echo    Tray icon       right click for menu
echo.
echo    To change keys later: tray - open config, edit
echo    [keys] list, then tray - reload config.
echo    No reinstall, no recompile.
echo.
echo    Author: ö°Ñôö«ÔÂ (ayjy)
echo  ============================================================
echo.
echo  ------------------------------------------------------------
echo      Author: ö°Ñôö«ÔÂ (ayjy)      KeyMacro
echo  ------------------------------------------------------------
echo.
echo       closing in 5 ...   by ö°Ñôö«ÔÂ (ayjy)
call :sleep1s
echo       closing in 4 ...   by ö°Ñôö«ÔÂ (ayjy)
call :sleep1s
echo       closing in 3 ...   by ö°Ñôö«ÔÂ (ayjy)
call :sleep1s
echo       closing in 2 ...   by ö°Ñôö«ÔÂ (ayjy)
call :sleep1s
echo       closing in 1 ...   by ö°Ñôö«ÔÂ (ayjy)
call :sleep1s
echo.
exit /b 0

rem ============================================================
rem  fake boot animation - decoration only, no real work
rem ============================================================
:bootanim
echo.
echo   ============================================================
echo     KeyMacro Installer
echo.
echo        Author: ö°Ñôö«ÔÂ (ayjy)
echo   ============================================================
echo.
echo   Initializing, please wait ...
echo.
echo       [##########                              ]  25%%   checking environment
call :sleep1s
echo       [####################                    ]  50%%   loading resources
call :sleep1s
echo       [##############################          ]  75%%   preparing components
call :sleep1s
echo       [########################################]  100%%  ready
call :sleep1s
echo.
goto :eof

:sleep1s
timeout /t 1 /nobreak >nul 2>&1
if errorlevel 1 ping -n 2 127.0.0.1 >nul 2>&1
goto :eof
