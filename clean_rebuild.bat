@echo off
setlocal enableDelayedExpansion

rem Clean and rebuild Moonlight Qt on Windows.
rem This script bootstraps the VS toolchain and Qt bin path if needed.

set "BUILD_CONFIG=%~1"
if "%BUILD_CONFIG%"=="" set "BUILD_CONFIG=release"

if /I "%BUILD_CONFIG%"=="debug" (
    set "BUILD_CONFIG=debug"
) else (
    if /I "%BUILD_CONFIG%"=="release" (
        set "BUILD_CONFIG=release"
    ) else (
        echo Invalid build configuration - expected "debug" or "release"
        echo Usage: clean_rebuild.bat [debug^|release]
        exit /b 1
    )
)

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "PUSHED=0"

echo Using repo root: %ROOT%
echo Preparing build environment.

set "VSWHERE=%ROOT%\scripts\vswhere.exe"
if not defined VCVARSALL_BAT (
    if exist "%VSWHERE%" (
        for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VS_INSTALL=%%i"
    )
    if defined VS_INSTALL set "VCVARSALL_BAT=!VS_INSTALL!\VC\Auxiliary\Build\vcvarsall.bat"
    if not defined VCVARSALL_BAT set "VCVARSALL_BAT=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
)

where cl.exe >nul 2>&1
if !ERRORLEVEL! NEQ 0 (
    if exist "%VCVARSALL_BAT%" (
        echo Initializing Visual Studio build environment
        call "%VCVARSALL_BAT%" x64
    ) else (
        echo Unable to find vcvarsall.bat. Set VCVARSALL_BAT to your VS install path.
        goto Error
    )
)

where cl.exe >nul 2>&1
if !ERRORLEVEL! NEQ 0 (
    echo Unable to find compiler 'cl' after running vcvarsall.bat.
    goto Error
)

if not defined QT_BIN_DIR set "QT_BIN_DIR=C:\Qt\6.10.1\msvc2022_64\bin"
where qmake.bat >nul 2>&1
if !ERRORLEVEL! NEQ 0 (
    where qmake.exe >nul 2>&1
    if !ERRORLEVEL! NEQ 0 (
        if exist "%QT_BIN_DIR%\qmake.exe" (
            set "PATH=%QT_BIN_DIR%;%PATH%"
        ) else if exist "%QT_BIN_DIR%\qmake.bat" (
            set "PATH=%QT_BIN_DIR%;%PATH%"
        )
    )
)

rem Locate qmake (prefer qmake6 if available)
where qmake6.bat >nul 2>&1
if !ERRORLEVEL! EQU 0 (
    set "QMAKE_CMD=qmake6.bat"
) else (
    where qmake6.exe >nul 2>&1
    if !ERRORLEVEL! EQU 0 (
        set "QMAKE_CMD=qmake6.exe"
    ) else (
        where qmake.bat >nul 2>&1
        if !ERRORLEVEL! EQU 0 (
            set "QMAKE_CMD=qmake.bat"
        ) else (
            where qmake.exe >nul 2>&1
            if !ERRORLEVEL! EQU 0 (
                set "QMAKE_CMD=qmake.exe"
            ) else (
                echo Unable to find QMake. Did you add Qt bin to your PATH?
                goto Error
            )
        )
    )
)

rem Locate build tool (prefer bundled jom)
if exist "%ROOT%\scripts\jom.exe" (
    set "MAKE_CMD=%ROOT%\scripts\jom.exe"
) else (
    where jom.exe >nul 2>&1
    if !ERRORLEVEL! EQU 0 (
        set "MAKE_CMD=jom.exe"
    ) else (
        where nmake.exe >nul 2>&1
        if !ERRORLEVEL! EQU 0 (
            set "MAKE_CMD=nmake.exe"
        ) else (
            echo Unable to find jom.exe or nmake.exe. Ensure Visual Studio build tools are in PATH.
            goto Error
        )
    )
)

echo Cleaning qmake artifacts
del /f /q "%ROOT%\Makefile" "%ROOT%\.qmake.cache" "%ROOT%\.qmake.stash" "%ROOT%\config.log" 2>nul
rmdir /s /q "%ROOT%\config.tests" 2>nul

for %%D in (moonlight-common-c qmdnsengine h264bitstream app AntiHooking) do (
    if exist "%ROOT%\%%D" (
        del /f /q "%ROOT%\%%D\Makefile" "%ROOT%\%%D\Makefile.Debug" "%ROOT%\%%D\Makefile.Release" "%ROOT%\%%D\.qmake.cache" "%ROOT%\%%D\.qmake.stash" 2>nul
        rmdir /s /q "%ROOT%\%%D\release" 2>nul
        rmdir /s /q "%ROOT%\%%D\debug" 2>nul
    )
)

del /f /q "%ROOT%\app\Moonlight.exe" 2>nul

echo Configuring project
pushd "%ROOT%"
set "PUSHED=1"
call "%QMAKE_CMD%" "%ROOT%\moonlight-qt.pro"
if !ERRORLEVEL! NEQ 0 goto Error

echo Building %BUILD_CONFIG%
call "%MAKE_CMD%" %BUILD_CONFIG%
if !ERRORLEVEL! NEQ 0 goto Error

echo Copying runtime DLLs
set "DLL_SRC=%ROOT%\libs\windows\lib\x64"
set "DLL_DST=%ROOT%\app\%BUILD_CONFIG%"
if not exist "%DLL_DST%" mkdir "%DLL_DST%"
if exist "%DLL_SRC%\*.dll" (
    for %%F in ("%DLL_SRC%\*.dll") do (
        copy /y "%%~fF" "%DLL_DST%" >nul
        if !ERRORLEVEL! NEQ 0 goto Error
    )
) else (
    echo Missing runtime DLLs in "%DLL_SRC%".
    goto Error
)

set "ANTIH_SRC=%ROOT%\AntiHooking\%BUILD_CONFIG%\AntiHooking.dll"
if exist "%ANTIH_SRC%" (
    copy /y "%ANTIH_SRC%" "%DLL_DST%" >nul
    if !ERRORLEVEL! NEQ 0 goto Error
) else (
    echo Missing AntiHooking.dll at "%ANTIH_SRC%".
    goto Error
)

popd
echo Build complete.
exit /b 0

:Error
set "EXIT_CODE=!ERRORLEVEL!"
if "%PUSHED%"=="1" popd
echo Build failed!
exit /b !EXIT_CODE!
