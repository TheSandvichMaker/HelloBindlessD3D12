@echo off

if "%1"=="clean" rmdir build /S /Q

rem ==========================================================================================
rem FLAGS
rem ==========================================================================================

set flags=/nologo /Z7 /WX /W4 /wd4201 /wd4115 /wd4013 /wd4116 /wd4324 /std:c++20 /DUNICODE=1 /D_CRT_SECURE_NO_WARNINGS
set debug_flags=/Od /MTd /DBUILD_DEBUG=1
set release_flags=/O2 /MT /DBUILD_RELEASE=1
set linker_flags=/opt:ref /incremental:no

rem ==========================================================================================
rem BUILD
rem ==========================================================================================

if not exist run   mkdir run
if not exist build mkdir build
pushd build

rem ==========================================================================================
rem DEBUG
rem ==========================================================================================

echo]
echo =========================
echo       DEBUG BUILD
echo =========================
echo]

cl ../hello_bindless.cpp /Fe:hello_bindless_debug.exe %flags% %debug_flags% /link %linker_flags%
if %ERRORLEVEL% neq 0 goto bail

robocopy . ..\run *_debug.exe *_debug.dll *_debug.pdb /S > NUL

rem ==========================================================================================
rem RELEASE
rem ==========================================================================================

echo]
echo =========================
echo      RELEASE BUILD
echo =========================
echo]

cl ../hello_bindless.cpp /Fe:hello_bindless_release.exe %flags% %release_flags% /link %linker_flags%
if %ERRORLEVEL% neq 0 goto bail

robocopy . ..\run *_release.exe *_release.dll *_release.pdb /S > NUL

:bail

popd