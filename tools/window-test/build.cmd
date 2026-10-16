@echo off
rem Run from a VS 2022 x64 Developer Command Prompt. Rebuilds the test exe the app bundles.
cl /nologo /c /O1 /GS- /Gs9999999 /Zl /Fo%TEMP%\window-test.obj %~dp0main.c || exit /b 1
link /nologo /NODEFAULTLIB /ENTRY:start /SUBSYSTEM:WINDOWS /DYNAMICBASE /OUT:%~dp0..\..\XboxPcHost\Games\window-test\window-test.exe %TEMP%\window-test.obj kernel32.lib user32.lib || exit /b 1
