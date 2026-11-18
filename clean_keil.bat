@echo off
setlocal EnableExtensions DisableDelayedExpansion
title Keil Build Cleanup

rem Place this file in your project root. All subfolders are included.
rem 0 = keep BIN/HEX/H86 firmware; 1 = delete them recursively too.
set "CLEAN_FIRMWARE=0"

pushd "%~dp0" || goto :path_error
set /a deleted=0, failed=0 >nul

echo Cleaning Keil build files under:
echo "%CD%"
echo.

rem Preserve source, project options, scatter files, libraries and backups.
rem Keep general HTM/HTML documents; only delete matching build logs.
for /r %%F in (*.o *.obj *.d *.crf *.axf *.elf *.lnp *.lst *.map *.dep *.sbr *.m51 *.m66 *.cod *.plg *.build_log.htm *._ac *._ia *.__i *._ii) do (
    if exist "%%~fF" if not exist "%%~fF\" (
        del /q "%%~fF" 2>nul
        if exist "%%~fF" (
            echo [FAILED] "%%~fF"
            set /a failed+=1 >nul
        ) else (
            set /a deleted+=1 >nul
        )
    )
)

if "%CLEAN_FIRMWARE%"=="1" (
    for /r %%F in (*.bin *.hex *.h86) do (
        if exist "%%~fF" if not exist "%%~fF\" (
            del /q "%%~fF" 2>nul
            if exist "%%~fF" (
                echo [FAILED] "%%~fF"
                set /a failed+=1 >nul
            ) else (
                set /a deleted+=1 >nul
            )
        )
    )
)

echo.
echo Deleted: %deleted% file(s). Failed: %failed% file(s).
echo Rebuild the project in Keil before debugging or downloading.
popd
if %failed% gtr 0 (
    echo Close Keil or check file permissions, then try again.
    pause
    exit /b 1
)
pause
exit /b 0

:path_error
echo Cannot open the script directory.
pause
exit /b 1
