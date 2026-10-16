@echo off
rem Run from a VS 2022 x64 Developer Command Prompt. Rebuilds the test exe the app bundles.
cl /nologo /c /O1 /GS- /Gs9999999 /Zl /Fo%TEMP%\loader-test.obj %~dp0main.c || exit /b 1
link /nologo /NODEFAULTLIB /ENTRY:start /SUBSYSTEM:CONSOLE /DYNAMICBASE /OUT:%~dp0..\..\XboxPcHost\Games\loader-test\loader-test.exe %TEMP%\loader-test.obj kernel32.lib || exit /b 1
