@echo off
rem Run from a VS 2022 x64 Developer Command Prompt. Rebuilds the test exe the app bundles.
cl /nologo /c /O1 /GS- /Gs9999999 /Zl /Fo%TEMP%\audio-test.obj %~dp0main.c || exit /b 1
link /nologo /NODEFAULTLIB /ENTRY:start /SUBSYSTEM:CONSOLE /DYNAMICBASE /OUT:%~dp0..\..\XboxPcHost\Games\audio-test\audio-test.exe %TEMP%\audio-test.obj kernel32.lib winmm.lib || exit /b 1
