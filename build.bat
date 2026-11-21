@echo off
rem Build md with the Visual Studio Build Tools (cl.exe) instead of GCC.
rem Everything goes into msvc\, under the names the Makefile uses.
rem
rem   build [target...]
rem     release   msvc\md.exe (the default)
rem     debug     msvc\md-debug.exe: debug info (a PDB), guard pages
rem     bench     msvc\md-bench.exe: a release build that takes --bench
rem     test      build the four test programs and run them
rem     clean     delete msvc\
rem     help      this list
rem
rem Works from any console: when cl.exe isn't on PATH, it finds the newest
rem Visual Studio or Build Tools install and runs its vcvars64.bat.
setlocal
cd /d "%~dp0"

set TARGETS=%*
if not defined TARGETS set TARGETS=release
for %%t in (%TARGETS%) do (
    echo " release debug bench test clean help " | findstr /i /c:" %%t " >nul || (
        echo build: unknown target "%%t"
        call :help
        exit /b 2
    )
)
for %%t in (%TARGETS%) do (
    call :%%t || exit /b 1
)
exit /b 0


:help
echo usage: build [release^|debug^|bench^|test^|clean^|help]...
exit /b 0

:clean
if exist msvc rmdir /s /q msvc
exit /b 0

:release
call :setup || exit /b 1
call :app md "%RELEASE%" -debug:none || exit /b 1
exit /b 0

:debug
call :setup || exit /b 1
call :app md-debug "%DEBUG%" -debug || exit /b 1
exit /b 0

:bench
call :setup || exit /b 1
call :app md-bench "%RELEASE% -DBENCH" -debug:none || exit /b 1
exit /b 0

:test
call :setup || exit /b 1
call :console md-test test.c kernel32.lib || exit /b 1
call :console hl-test hltest.c kernel32.lib || exit /b 1
call :console view-test viewtest.c kernel32.lib || exit /b 1
call :console img-test imgtest.c "kernel32.lib ole32.lib" || exit /b 1
for %%p in (md-test hl-test view-test img-test) do (
    msvc\%%p.exe || exit /b 1
)
exit /b 0


rem The windowed app: :app NAME "COMPILER FLAGS" LINKER-DEBUG-FLAG
:app
cl %CFLAGS% %~2 -Fo:msvc\%1.obj -Fd:msvc\%1.pdb -Fe:msvc\%1.exe main.c ^
    msvc\md.res -link %LDFLAGS% %3 -subsystem:windows ^
    -entry:WinMainCRTStartup %LIBS% || exit /b 1
exit /b 0

rem A test program, always with the debug checks: :console NAME SOURCE "LIBS"
:console
cl %CFLAGS% %DEBUG% -Fo:msvc\%1.obj -Fd:msvc\%1.pdb -Fe:msvc\%1.exe %2 ^
    -link %LDFLAGS% -debug -subsystem:console -entry:mainCRTStartup ^
    %~3 msvc\ntdll-crt.lib || exit /b 1
exit /b 0


rem Find the tools and make what every build links. Runs once.
:setup
if defined SETUP_DONE exit /b 0
where /q cl.exe || call :vcvars || exit /b 1
cd /d "%~dp0"
if not exist msvc mkdir msvc

rem No C runtime, as with GCC. GCC's -Wconversion build already checks
rem conversions; these are the MSVC warnings it doesn't share: int to float,
rem sign changes, unary minus on unsigned, a const forward declaration, and
rem noreturn with a return type.
set CFLAGS=-nologo -std:c11 -utf-8 -W4 -GS- -Zl ^
    -wd4244 -wd4245 -wd4146 -wd4132 -wd4646
set RELEASE=-O2 -Gw -Gy
set DEBUG=-Od -Zi -DDEBUG
rem A 2 MB stack, like GCC's default.
set LDFLAGS=-nologo -nodefaultlib -stack:0x200000 -opt:ref -opt:icf
set LIBS=kernel32.lib user32.lib gdi32.lib ole32.lib advapi32.lib ^
    shell32.lib comdlg32.lib dwmapi.lib d2d1.lib dwrite.lib msvc\ntdll-crt.lib

rem ntdll.dll exports setjmp, longjmp and the stack probe, but the Windows
rem SDK's ntdll.lib leaves them out (see md.c), so make an import library.
> msvc\ntdll-crt.def (echo LIBRARY ntdll.dll& echo EXPORTS& echo _setjmp& echo longjmp& echo __chkstk)
lib -nologo -machine:x64 -def:msvc\ntdll-crt.def -out:msvc\ntdll-crt.lib >nul || exit /b 1
rc -nologo -i res -fo msvc\md.res res\md.rc || exit /b 1
set SETUP_DONE=1
exit /b 0

:vcvars
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo build: cl.exe is not on PATH and Visual Studio is not installed
    exit /b 1
)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo build: no Visual Studio install has the C++ build tools
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
exit /b
