@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1
cd /d "%~dp0"
if not exist bin mkdir bin
if not exist obj mkdir obj
set CF=/nologo /std:c++20 /O2 /MT /EHsc /W4 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /Foobj\

cl %CF% /LD src\dll\multilink_dll.cpp /Fe:bin\multilink.dll || exit /b 1
cl %CF% src\injector\injector.cpp /Fe:bin\multilink_inject.exe /link /MANIFESTUAC:"level='requireAdministrator'" shell32.lib || exit /b 1
cl %CF% tests\selftest.cpp /Fe:obj\selftest.exe || exit /b 1
obj\selftest.exe || exit /b 1
rem Keep the user's edited config; only seed it on first build.
if not exist bin\multilink.ini copy multilink.ini bin\ >nul
echo build ok
