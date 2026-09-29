@echo off
REM ahcint.sys for NT 3.50/3.51/4.0. MSVCDIR = Visual C++ 4.0, DDKDIR = NT4 DDK

set MSVCDIR=C:\MSDEV
set DDKDIR=C:\NT4DDK

set PATH=%MSVCDIR%\BIN;%DDKDIR%\BIN;%PATH%
set INCLUDE=%DDKDIR%\inc;%DDKDIR%\src\storage\inc;%DDKDIR%\inc\crt;%MSVCDIR%\INCLUDE;C:\HYBRIDST\inc
set LIB=%DDKDIR%\lib\i386;%MSVCDIR%\LIB;%DDKDIR%\lib\i386\free;%LIB%

if exist ahcimain.obj del ahcimain.obj
if exist ahcisatl.obj del ahcisatl.obj
if exist ahcint.sys del ahcint.sys

cl -nologo -c -Gz -Ox -W3 -Zp8 -Zi -DAHCI_NT4 -D_X86_=1 -Di386=1 -DCONDITION_HANDLING=1 -DNT_UP=1 -DNT_INST=0 -DWIN32=100 -D_NT1X_=100 -DWINNT=1 -D_WIN32_WINNT=0x0350 /I.. /I..\inc ..\ahcimain.c ..\ahcisatl.c
if errorlevel 1 goto error

link -nologo -debug -debugtype:both -subsystem:native,3.50 -entry:DriverEntry@8 -driver -base:0x10000 -align:0x200 -out:ahcint.sys ahcimain.obj ahcisatl.obj scsiport.lib ntoskrnl.lib
if errorlevel 1 goto error

echo ahcint.sys built.
goto end

:error
echo Build FAILED.

:end
