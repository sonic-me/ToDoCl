@echo off
REM ===========================================================================
REM  Builds TaskTicket.exe using the MSVC command-line compiler (cl.exe).
REM  Requires ONLY the "Build Tools for Visual Studio" (free download from
REM  Microsoft) - NOT the full Visual Studio IDE.
REM
REM  1. Install "Build Tools for Visual Studio 2022":
REM     https://visualstudio.microsoft.com/visual-cpp-build-tools/
REM     During install, select the "Desktop development with C++" workload.
REM
REM  2. Open "x64 Native Tools Command Prompt for VS 2022" from the Start
REM     Menu (this sets up cl.exe, link.exe, rc.exe on PATH), cd into this
REM     folder, then run:
REM         build.bat
REM
REM  Output: build\TaskTicket.exe
REM ===========================================================================

setlocal
set OUTDIR=build
if not exist %OUTDIR% mkdir %OUTDIR%

where cl.exe >nul 2>nul
if errorlevel 1 (
    echo ERROR: cl.exe not found on PATH.
    echo Please run this script from an "x64 Native Tools Command Prompt for VS".
    exit /b 1
)

echo Compiling resource script...
rc.exe /fo %OUTDIR%\TaskTicket.res TaskTicket.rc
if errorlevel 1 goto :error

echo Compiling and linking...
cl.exe /nologo /std:c++17 /EHsc /O2 /W4 /DUNICODE /D_UNICODE ^
    /Fo%OUTDIR%\ /Fe%OUTDIR%\TaskTicket.exe ^
    WinMain.cpp TaskTicketApp.cpp TaskTicketInput.cpp Persistence.cpp ^
    %OUTDIR%\TaskTicket.res ^
    /link /SUBSYSTEM:WINDOWS ^
    dwmapi.lib shell32.lib ole32.lib user32.lib gdi32.lib
if errorlevel 1 goto :error

echo.
echo Build succeeded: %OUTDIR%\TaskTicket.exe
endlocal
exit /b 0

:error
echo.
echo Build FAILED.
endlocal
exit /b 1
