# Dialog Box Assistant - Windows 11 port

A Windows 11 port of Dialog Box Assistant: a tray application with options window, hotkey, and
hook-based remembering of dialog folders (64-bit hook DLL plus a 32-bit helper process).

## Build

Requires Visual Studio 2022 Community (C++ tools). Run:

    build.bat

Output goes to `bin/` (DBA.exe, DBA32.exe, DBAHook32.dll, DBAHook64.dll). Keep these files together.
