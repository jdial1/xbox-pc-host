@echo off
rem Run from a VS 2022 x64 Developer Command Prompt. Rebuilds the test exe the app bundles.
cl /nologo /c /O1 /GS- /Gs9999999 /Zl /Fo%TEMP%\gdi-test.obj %~dp0main.c || exit /b 1
link /nologo /NODEFAULTLIB /ENTRY:start /SUBSYSTEM:WINDOWS /DYNAMICBASE /OUT:%~dp0..\..\XboxPcHost\Games\gdi-test\gdi-test.exe %TEMP%\gdi-test.obj kernel32.lib user32.lib gdi32.lib || exit /b 1
