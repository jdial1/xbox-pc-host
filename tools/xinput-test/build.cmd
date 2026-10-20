@rem From a VS x64 developer prompt.
cl /nologo /O2 /MT /Fe:%~dp0..\..\XboxPcHost\Games\xinput-test\xinput-test.exe /Fo:%TEMP%\ %~dp0main.c xinput.lib
