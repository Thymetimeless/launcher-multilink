@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1
cd /d "%~dp0"
if not exist obj mkdir obj
set CF=/nologo /std:c++20 /O2 /MT /EHsc /W4 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /Foobj\

rem multilink.dll, multilink_inject.exe and multilink.ini end up side by side in this folder.
rem A running launcher may still hold the previous DLL: Windows lets a loaded DLL be renamed, not overwritten.
del /q obj\*.old.dll >nul 2>&1
if exist multilink.dll move /y multilink.dll obj\multilink.%RANDOM%.old.dll >nul || exit /b 1
cl %CF% /LD src\dll\multilink_dll.cpp /Fe:multilink.dll /link /IMPLIB:obj\multilink.lib || exit /b 1
cl %CF% src\injector\injector.cpp /Fe:multilink_inject.exe /link /MANIFESTUAC:"level='requireAdministrator'" shell32.lib advapi32.lib || exit /b 1
cl %CF% tests\selftest.cpp /Fe:obj\selftest.exe || exit /b 1
obj\selftest.exe || exit /b 1
rem Keep the user's edited config; only seed it on first build.
if not exist multilink.ini copy multilink.example.ini multilink.ini >nul
echo build ok
