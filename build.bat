@echo off
setlocal enabledelayedexpansion

REM Builds dist\dbghelp.dll as a 32-bit dbghelp proxy.

set PROJECT_ROOT=%~dp0

REM Locate vcvarsall.bat via vswhere, then common VS paths.
set VCVARSALL=
set "PF=%ProgramFiles%"
set "PFX86=%ProgramFiles(x86)%"
set "VSWHERE=!PFX86!\Microsoft Visual Studio\Installer\vswhere.exe"

if exist "!VSWHERE!" (
    for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do (
        if exist "%%i\VC\Auxiliary\Build\vcvarsall.bat" set "VCVARSALL=%%i\VC\Auxiliary\Build\vcvarsall.bat"
    )
)

if "!VCVARSALL!"=="" (
    for %%E in (BuildTools Community Professional Enterprise) do (
        for %%V in (2022 2019) do (
            if exist "!PF!\Microsoft Visual Studio\%%V\%%E\VC\Auxiliary\Build\vcvarsall.bat" (
                set "VCVARSALL=!PF!\Microsoft Visual Studio\%%V\%%E\VC\Auxiliary\Build\vcvarsall.bat"
            )
            if exist "!PFX86!\Microsoft Visual Studio\%%V\%%E\VC\Auxiliary\Build\vcvarsall.bat" (
                set "VCVARSALL=!PFX86!\Microsoft Visual Studio\%%V\%%E\VC\Auxiliary\Build\vcvarsall.bat"
            )
        )
    )
)

if "!VCVARSALL!"=="" goto :no_vcvars
if not exist "!VCVARSALL!" goto :no_vcvars

call "!VCVARSALL!" x86 >nul
if errorlevel 1 goto :vcvars_fail

cd /d "%PROJECT_ROOT%"

if not exist build mkdir build
if not exist dist  mkdir dist

REM Compile MinHook (C sources)
echo [1/3] Compiling MinHook...
set MH_DIR=thirdparty\minhook\src
set MH_INC=thirdparty\minhook\include

cl /nologo /c /O2 /MT /W3 /D_WIN32_WINNT=0x0601 /I"!MH_INC!" /I"!MH_DIR!" /Fo"build\\" "!MH_DIR!\buffer.c" "!MH_DIR!\hook.c" "!MH_DIR!\trampoline.c" "!MH_DIR!\hde\hde32.c" "!MH_DIR!\hde\hde64.c"
if errorlevel 1 goto :minhook_fail

REM Auto-discover project sources : recursive over src\.
REM .obj names are path-encoded so basenames duplicated across subfolders don't collide.
echo [2/3] Compiling proxy DLL sources...

set CL_FLAGS=/nologo /c /O2 /MT /W3 /EHsc /std:c++17 /D_WIN32_WINNT=0x0601 /I"src" /I"!MH_INC!"

for /r src %%f in (*.cpp) do (
    set "rel=%%f"
    set "rel=!rel:%PROJECT_ROOT%=!"
    set "rel=!rel:\=_!"
    set "rel=!rel:.cpp=.obj!"
    cl !CL_FLAGS! /Fo"build\!rel!" "%%f"
    if errorlevel 1 goto :project_fail
)

REM Link as DLL named dbghelp.dll.
echo [3/3] Linking dbghelp.dll proxy...

set OBJ_LIST=
for /r src %%f in (*.cpp) do (
    set "rel=%%f"
    set "rel=!rel:%PROJECT_ROOT%=!"
    set "rel=!rel:\=_!"
    set "rel=!rel:.cpp=.obj!"
    set OBJ_LIST=!OBJ_LIST! "build\!rel!"
)

set TP_OBJS=build\buffer.obj build\hook.obj build\trampoline.obj build\hde32.obj build\hde64.obj

link /nologo /DLL /MACHINE:X86 /SUBSYSTEM:WINDOWS /OUT:"dist\dbghelp.dll" /PDB:"dist\dbghelp.pdb" /OPT:REF /OPT:ICF !OBJ_LIST! !TP_OBJS! kernel32.lib user32.lib advapi32.lib
if errorlevel 1 goto :link_fail

echo.
echo ================================================================
echo Build complete : dist\dbghelp.dll
echo ================================================================
dir dist\dbghelp.dll
echo.
echo To install :
echo   Copy dist\dbghelp.dll into the game folder ^(next to mowas_2.exe^).
echo   The proxy resolves the real system dbghelp.dll at runtime.
echo.

endlocal
exit /b 0

:no_vcvars
echo ERROR: could not locate vcvarsall.bat for any Visual Studio install.
echo Looked for vswhere at "!VSWHERE!" then VS 2022/2019 BuildTools/Community/Professional/Enterprise
echo under both %%ProgramFiles%% and %%ProgramFiles(x86)%%.
echo Install Visual Studio Build Tools or set VCVARSALL=... before calling this script.
endlocal
exit /b 1

:vcvars_fail
echo ERROR: vcvarsall.bat x86 setup failed
endlocal
exit /b 1

:minhook_fail
echo ERROR: MinHook compilation failed
endlocal
exit /b 1

:project_fail
echo ERROR: Project compilation failed
endlocal
exit /b 1

:link_fail
echo ERROR: Link failed
endlocal
exit /b 1
