@echo off
chcp 936 >nul 2>&1
setlocal EnableDelayedExpansion
title KeyMacro Uninstaller  -  by ö°Ñôö«ÔÂ (ayjy)
cd /d "%~dp0"

echo.
echo  ============================================================
echo    KeyMacro  /  Uninstaller
echo    Remove: program  ^|  build env  ^|  downloaded files
echo    Author: ö°Ñôö«ÔÂ (ayjy)
echo  ============================================================
echo.
call :uboot

rem ---------- [0] admin ----------
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo  [!] Requesting administrator privilege...
    echo.
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)
echo  [OK] Administrator
echo.

rem ---------- [1] menu ----------
echo  ------------------------------------------------------------
echo    What do you want to remove?
echo  ------------------------------------------------------------
echo.
echo    [1] KeyMacro program only          (RECOMMENDED)
echo        - desktop exe, startup entry, config, build output
echo        - MSYS2 / compiler is KEPT
echo.
echo    [2] Program + MSYS2 build environment
echo        - everything in [1]
echo        - plus C:\msys64 (about 1-3 GB)
echo.
echo    [3] Everything  (program + MSYS2 + download cache)
echo        - everything in [2]
echo        - plus pacman cache and installer temp files
echo.
echo    [4] MSYS2 environment only         (keep the program)
echo.
echo    [0] Cancel
echo.
echo    ---------------------------------------------------
echo       KeyMacro  by  ö°Ñôö«ÔÂ (ayjy)
echo    ---------------------------------------------------
echo.
echo  ------------------------------------------------------------
set "CHOICE="
set /p "CHOICE=  Enter number (0-4): "

if "!CHOICE!"=="0" goto :cancel
if "!CHOICE!"=="1" goto :do_prog
if "!CHOICE!"=="2" goto :do_prog_env
if "!CHOICE!"=="3" goto :do_all
if "!CHOICE!"=="4" goto :do_env
echo.
echo  [!] Invalid input.
pause
exit /b

:cancel
echo.
echo  Cancelled. Nothing was changed.
echo.
pause
exit /b

:do_prog
set "DO_PROG=1"
set "DO_ENV=0"
set "DO_CACHE=0"
goto :confirm

:do_prog_env
set "DO_PROG=1"
set "DO_ENV=1"
set "DO_CACHE=0"
goto :confirm

:do_all
set "DO_PROG=1"
set "DO_ENV=1"
set "DO_CACHE=1"
goto :confirm

:do_env
set "DO_PROG=0"
set "DO_ENV=1"
set "DO_CACHE=0"
goto :confirm

:confirm
echo.
echo  ------------------------------------------------------------
echo    About to remove:
if "!DO_PROG!"=="1" echo      - KeyMacro program, config, startup entry
if "!DO_ENV!"=="1"  echo      - MSYS2 build environment
if "!DO_CACHE!"=="1" echo     - download cache and installer temp files
echo  ------------------------------------------------------------
echo.
set "YN="
set /p "YN=  Type YES to continue: "
if /i not "!YN!"=="YES" (
    echo.
    echo  Cancelled. Nothing was changed.
    echo.
    pause
    exit /b
)

rem ============================================================
echo.
echo  ============================================================
echo    Step 1 / 5 : Stop running process
echo  ============================================================
taskkill /F /IM KeyMacro.exe /T >nul 2>&1
if %errorlevel% equ 0 (echo      - KeyMacro.exe stopped) else (echo      - not running)
echo.

rem ============================================================
echo  ============================================================
echo    Step 2 / 5 : Remove program files
echo  ============================================================
if not "!DO_PROG!"=="1" (
    echo      - skipped
    echo.
    goto :step3
)

rem --- desktop (current user) ---
if exist "%USERPROFILE%\Desktop\KeyMacro.exe" (
    del /F /Q "%USERPROFILE%\Desktop\KeyMacro.exe" >nul 2>&1
    echo      - Desktop\KeyMacro.exe
)
rem --- desktop (OneDrive) ---
if exist "%USERPROFILE%\OneDrive\Desktop\KeyMacro.exe" (
    del /F /Q "%USERPROFILE%\OneDrive\Desktop\KeyMacro.exe" >nul 2>&1
    echo      - OneDrive\Desktop\KeyMacro.exe
)
rem --- desktop (public / all users) ---
if defined PUBLIC (
    if exist "%PUBLIC%\Desktop\KeyMacro.exe" (
        del /F /Q "%PUBLIC%\Desktop\KeyMacro.exe" >nul 2>&1
        echo      - Public\Desktop\KeyMacro.exe
    )
)
rem --- startup folder ---
if exist "%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\KeyMacro.exe" (
    del /F /Q "%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\KeyMacro.exe" >nul 2>&1
    echo      - Startup\KeyMacro.exe
)
if defined ProgramData (
    if exist "%ProgramData%\Microsoft\Windows\Start Menu\Programs\Startup\KeyMacro.exe" (
        del /F /Q "%ProgramData%\Microsoft\Windows\Start Menu\Programs\Startup\KeyMacro.exe" >nul 2>&1
        echo      - AllUsers Startup\KeyMacro.exe
    )
)
rem --- registry Run ---
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v "KeyMacro" /f >nul 2>&1
if %errorlevel% equ 0 echo      - Registry Run (HKCU)
reg delete "HKLM\Software\Microsoft\Windows\CurrentVersion\Run" /v "KeyMacro" /f >nul 2>&1
if %errorlevel% equ 0 echo      - Registry Run (HKLM)

rem --- config dir ---
if exist "%APPDATA%\KeyMacro" (
    rmdir /S /Q "%APPDATA%\KeyMacro" >nul 2>&1
    echo      - %%APPDATA%%\KeyMacro  (config)
)

rem --- build output in this folder ---
if exist "KeyMacro.exe" (
    del /F /Q "KeyMacro.exe" >nul 2>&1
    echo      - .\KeyMacro.exe
)
if exist "probe.exe" (
    del /F /Q "probe.exe" >nul 2>&1
    echo      - .\probe.exe
)
if exist "timer_report.txt" (
    del /F /Q "timer_report.txt" >nul 2>&1
    echo      - .\timer_report.txt
)
if exist "build.bat.log" del /F /Q "build.bat.log" >nul 2>&1
echo.

rem ============================================================
:step3
echo  ============================================================
echo    Step 3 / 5 : Remove source code folder
echo  ============================================================
set "YN2="
set /p "YN2=  Also delete the source folder (src\, *.bat, *.ps1)? [y/N]: "
if /i "!YN2!"=="y" (
    if exist "src" (
        rmdir /S /Q "src" >nul 2>&1
        echo      - src\
    )
    for %%F in (install.bat get-msys2.ps1 pick_keys.ps1 README.txt build.bat build-mingw.bat) do (
        if exist "%%F" (
            del /F /Q "%%F" >nul 2>&1
            echo      - %%F
        )
    )
    echo      - done
) else (
    echo      - kept
)
echo.

rem ============================================================
echo  ============================================================
echo    Step 4 / 5 : Remove MSYS2 build environment
echo  ============================================================
if not "!DO_ENV!"=="1" (
    echo      - skipped
    echo.
    goto :step5
)

rem stop anything still holding files
taskkill /F /IM pacman.exe >nul 2>&1
taskkill /F /IM bash.exe >nul 2>&1
taskkill /F /IM mintty.exe >nul 2>&1
taskkill /F /IM g++.exe >nul 2>&1

set "MSYS_FOUND=0"
for %%D in (C:\msys64 C:\msys2 C:\msys D:\msys64 D:\msys2 %SystemDrive%\msys64 %LOCALAPPDATA%\msys64) do (
    if exist "%%D\ucrt64\bin\g++.exe" (
        set "MSYS_FOUND=1"
        echo      - found MSYS2 at %%D
        set "MSYS_DIR=%%D"
    ) else if exist "%%D\msys2_shell.cmd" (
        if "!MSYS_FOUND!"=="0" (
            set "MSYS_FOUND=1"
            echo      - found MSYS2 at %%D
            set "MSYS_DIR=%%D"
        )
    )
)

if "!MSYS_FOUND!"=="0" (
    echo      - MSYS2 not found in common locations.
    set "CUSTOM="
    set /p "CUSTOM=  Enter full path manually (or press Enter to skip): "
    if defined CUSTOM (
        if exist "!CUSTOM!" (
            set "MSYS_DIR=!CUSTOM!"
            set "MSYS_FOUND=1"
        )
    )
)

if "!MSYS_FOUND!"=="1" (
    if "!DO_CACHE!"=="1" (
        if exist "!MSYS_DIR!\var\cache\pacman\pkg" (
            rmdir /S /Q "!MSYS_DIR!\var\cache\pacman\pkg" >nul 2>&1
            echo      - pacman cache removed
        )
    )
    echo      - removing !MSYS_DIR!  (this may take a while...)
    rmdir /S /Q "!MSYS_DIR!" >nul 2>&1
    if exist "!MSYS_DIR!" (
        echo      - [!] Some files could not be deleted (still in use).
        echo        Reboot and run this tool again.
    ) else (
        echo      - MSYS2 removed
    )
) else (
    echo      - nothing to remove
)

rem --- winget / start menu entries ---
if exist "%LOCALAPPDATA%\Microsoft\Windows\Apps\MSYS2" (
    rmdir /S /Q "%LOCALAPPDATA%\Microsoft\Windows\Apps\MSYS2" >nul 2>&1
)
if exist "%ProgramData%\Microsoft\Windows\Start Menu\Programs\MSYS2" (
    rmdir /S /Q "%ProgramData%\Microsoft\Windows\Start Menu\Programs\MSYS2" >nul 2>&1
    echo      - Start Menu\MSYS2
)
echo.

rem ============================================================
:step5
echo  ============================================================
echo    Step 5 / 5 : Remove downloaded installer + temp files
echo  ============================================================
if not "!DO_CACHE!"=="1" (
    echo      - skipped
    echo.
    goto :done
)
set "CNT=0"
for %%P in ("%TEMP%" "%SystemRoot%\Temp" "%USERPROFILE%\Downloads") do (
    if exist %%P (
        for %%F in ("%%~P\msys2-x86_64-*.exe" "%%~P\msys2-*.exe") do (
            if exist %%F (
                del /F /Q %%F >nul 2>&1
                echo      - %%~F
            )
        )
    )
)
echo      - done
echo.

rem ============================================================
:done
echo  ============================================================
echo    Uninstall finished.
echo.
echo    Thank you for using KeyMacro.
echo    Author: ö°Ñôö«ÔÂ
echo  ============================================================
echo.
echo  What was kept:
echo    - Windows system files          (untouched)
echo    - your personal files           (untouched)
if "!DO_ENV!"=="0" echo    - MSYS2 / MinGW compiler    (still installed)
if "!DO_PROG!"=="0" echo   - KeyMacro program          (still installed)
echo.
echo  To reinstall, just run install.bat again.
echo.
echo  ------------------------------------------------------------
echo      Author: ö°Ñôö«ÔÂ (ayjy)
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
exit /b

rem ============================================================
:uboot
echo   Initializing, please wait ...
echo.
echo       Author: ö°Ñôö«ÔÂ (ayjy)
echo.
echo       [##########                              ]  25%%   scanning installation
call :sleep1s
echo       [####################                    ]  50%%   reading registry
call :sleep1s
echo       [##############################          ]  75%%   building file list
call :sleep1s
echo       [########################################]  100%%  ready
echo.
goto :eof

:sleep1s
timeout /t 1 /nobreak >nul 2>&1
if errorlevel 1 ping -n 2 127.0.0.1 >nul 2>&1
goto :eof
