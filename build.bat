@echo off
setlocal
cd /d "%~dp0"
if not exist bin mkdir bin
if not exist obj\x64 mkdir obj\x64
if not exist obj\x64m mkdir obj\x64m
if not exist obj\x86 mkdir obj\x86
if not exist obj\x86h mkdir obj\x86h
if not exist old mkdir old
rem Loaded hook DLLs cannot be overwritten but can be renamed out of the way.
if exist bin\DBAHook64.dll move /y bin\DBAHook64.dll old\DBAHook64.dll.%RANDOM% >nul
if exist bin\DBAHook32.dll move /y bin\DBAHook32.dll old\DBAHook32.dll.%RANDOM% >nul
set "VS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build"

call "%VS%\vcvars64.bat" >nul 2>&1
cl /nologo /O2 /MT /EHsc /W3 /utf-8 /LD src\hook.cpp src\common.cpp /Fo:obj\x64\ /Fe:bin\DBAHook64.dll /link /DLL >obj\hook64.log || (type obj\hook64.log & exit /b 1)
rc /nologo /fo obj\res64.res res\dba.rc || exit /b 1
cl /nologo /O2 /MT /EHsc /W3 /utf-8 src\main.cpp src\common.cpp obj\res64.res /Fo:obj\x64m\ /Fe:bin\DBA.exe /link /SUBSYSTEM:WINDOWS >obj\main64.log || (type obj\main64.log & exit /b 1)

call "%VS%\vcvars32.bat" >nul 2>&1
cl /nologo /O2 /MT /EHsc /W3 /utf-8 /LD src\hook.cpp src\common.cpp /Fo:obj\x86\ /Fe:bin\DBAHook32.dll /link /DLL >obj\hook32.log || (type obj\hook32.log & exit /b 1)
cl /nologo /O2 /MT /EHsc /W3 /utf-8 src\host32.cpp /Fo:obj\x86h\ /Fe:bin\DBA32.exe /link /SUBSYSTEM:WINDOWS >obj\host32.log || (type obj\host32.log & exit /b 1)
echo BUILD OK
