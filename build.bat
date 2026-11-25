@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

echo ============================================================================
echo   Ribbon 2.0 - Windows 11 Compiler ^& Setup Builder
echo ============================================================================
echo.

if not exist dist mkdir dist

set "BUILT=0"

:: 1. Try MinGW-w64 (g++)
where g++ >nul 2>nul
if %ERRORLEVEL%==0 (
    echo [1/2] Found MinGW-w64 g++! Compiling Ribbon.exe...
    where windres >nul 2>nul
    if !ERRORLEVEL!==0 (
        windres ribbon.rc -O coff -o dist\ribbon.res
        g++ -std=c++17 -O2 -mwindows -municode -static-libgcc -static-libstdc++ ribbon.cpp dist\ribbon.res -o dist\Ribbon.exe -lwinmm -lgdi32 -luser32 -lcomdlg32 -lshell32
    ) else (
        g++ -std=c++17 -O2 -mwindows -municode -static-libgcc -static-libstdc++ ribbon.cpp -o dist\Ribbon.exe -lwinmm -lgdi32 -luser32 -lcomdlg32 -lshell32
    )
    if !ERRORLEVEL!==0 set "BUILT=1"
)

:: 2. Try MSVC (cl.exe already in PATH or located via vswhere)
if "%BUILT%"=="0" (
    where cl >nul 2>nul
    if !ERRORLEVEL! NEQ 0 (
        set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
        if exist "!VSWHERE!" (
            for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
                if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" (
                    echo [1/2] Loading Visual Studio x64 build tools...
                    call "%%i\VC\Auxiliary\Build\vcvars64.bat" >nul
                )
            )
        )
    )
    where cl >nul 2>nul
    if !ERRORLEVEL!==0 (
        echo [1/2] Found MSVC cl.exe! Compiling Ribbon.exe...
        rc /nologo /fo dist\ribbon.res ribbon.rc
        cl /nologo /std:c++17 /EHsc /O2 /MT /Fe:dist\Ribbon.exe ribbon.cpp dist\ribbon.res user32.lib gdi32.lib winmm.lib comdlg32.lib shell32.lib /link /SUBSYSTEM:WINDOWS
        if !ERRORLEVEL!==0 (
            del /q ribbon.obj 2>nul
            set "BUILT=1"
        )
    )
)

if "%BUILT%"=="0" (
    echo.
    echo [ERROR] No C++ compiler ^(MinGW-w64 or Visual Studio Build Tools^) was found.
    echo.
    echo To install MinGW-w64 quickly on Windows 11, open PowerShell and run:
    echo     winget install -e --id BrechtSanders.WinLibs.POSIX.UCRT
    echo.
    echo Then close and re-open this window and double-click build.bat again.
    pause
    exit /b 1
)

echo.
echo [OK] Successfully built: dist\Ribbon.exe

:: 3. Try Inno Setup 6 (ISCC.exe) to build RibbonSetup.exe
set "ISCC="
where ISCC >nul 2>nul && set "ISCC=ISCC"
if "%ISCC%"=="" if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if "%ISCC%"=="" if exist "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"

if not "%ISCC%"=="" (
    echo [2/2] Found Inno Setup 6! Building Windows 11 installer dist\RibbonSetup.exe...
    "%ISCC%" RibbonSetup.iss
    if !ERRORLEVEL!==0 (
        echo.
        echo ============================================================================
        echo   DONE! Your files are ready in the "dist" folder:
        echo     - dist\Ribbon.exe       ^(standalone portable app^)
        echo     - dist\RibbonSetup.exe  ^(full Windows 11 setup installer^)
        echo ============================================================================
    )
) else (
    echo.
    echo [2/2] Inno Setup 6 not found ^(optional^).
    echo       - Standalone app is ready right now: dist\Ribbon.exe
    echo       - To install Ribbon into Start Menu + .rib file association immediately,
    echo         right-click "Install-Ribbon-Portable.ps1" -^> "Run with PowerShell".
    echo       - Or install Inno Setup 6 to generate RibbonSetup.exe:
    echo           winget install -e --id JRSoftware.InnoSetup
)

echo.
pause
