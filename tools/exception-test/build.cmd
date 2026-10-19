@rem From a VS x64 developer prompt. Static CRT, like OpenTTD's release build.
cl /nologo /O2 /EHsc /MT /Fe:%~dp0..\..\XboxPcHost\Games\exception-test\exception-test.exe /Fo:%TEMP%\ %~dp0main.cpp
