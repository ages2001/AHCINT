@echo off
REM ============================================================
REM  ahcint9x Win95 miniport build script (DOS/COMMAND.COM syntax --
REM  Windows 95's batch interpreter does NOT understand the
REM  multi-line "if not exist (...)" block syntax that modern
REM  cmd.exe supports. Every check below is a single-line "if".
REM
REM  Assumes:
REM    C:\ahcint9x   - this driver's source (ahcint9x.h, ahcimain.c,
REM                  ahcisatl.c, makefile, ahcint9x.lnk)
REM    C:\DDK      - Windows 95 DDK root (contains MASTER.MK, BLOCK,
REM                  INC32, LIB)
REM    C:\MSTOOLS  - the Win32 SDK for NT4/Win95
REM    C:\MASM611  - MASM 6.11 install (BIN\ML.EXE, BIN\NMAKE.EXE)
REM    C:\MSVC20   - Visual C++ 2.0 install (BIN\CL.EXE, BIN\NMAKE.EXE)
REM
REM  Adjust the SET lines below if any of these live somewhere else.
REM ============================================================

set MASM_ROOT=C:\MASM611
set C16_ROOT=C:\MSVC20
set C32_ROOT=C:\MSVC20
set SDKROOT=C:\MSTOOLS
set DDKROOT=C:\DDK

set MASTER_MAKE=1

REM MASTER.MK requires TMP/TEMP to already exist. If not set, default
REM to C:\WINDOWS\TEMP (create it if it isn't there).
if "%TMP%"=="" set TMP=C:\WINDOWS\TEMP
if "%TEMP%"=="" set TEMP=C:\WINDOWS\TEMP
if not exist "%TMP%" mkdir "%TMP%"
if not exist "%TEMP%" mkdir "%TEMP%"

REM nmake.exe itself needs to be on PATH before MASTER.MK's own PATH
REM setup runs (that setup only takes effect once nmake is already
REM running and processing the makefile). Both MASM611\BIN and
REM MSVC20\BIN ship their own NMAKE.EXE -- put both on PATH so it's
REM found regardless of which copy MASTER.MK expects tools to be
REM invoked alongside.
set PATH=%MASM_ROOT%\BIN;%C32_ROOT%\BIN;%PATH%

if not exist "%MASM_ROOT%\BIN\ML.EXE" echo [build.bat] WARNING: %MASM_ROOT%\BIN\ML.EXE not found -- MASM_ROOT looks wrong.
if not exist "%C32_ROOT%\BIN\CL.EXE" echo [build.bat] WARNING: %C32_ROOT%\BIN\CL.EXE not found -- C32_ROOT looks wrong.
if not exist "%DDKROOT%\MASTER.MK" echo [build.bat] WARNING: %DDKROOT%\MASTER.MK not found -- DDKROOT looks wrong.
if not exist "%MASM_ROOT%\BIN\NMAKE.EXE" if not exist "%C32_ROOT%\BIN\NMAKE.EXE" echo [build.bat] WARNING: NMAKE.EXE not found under MASM_ROOT or C32_ROOT.

cd \ahcint9x
nmake