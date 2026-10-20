@rem From a VS x64 developer prompt.
cl /nologo /O2 /EHsc /MT /Fe:%~dp0..\..\XboxPcHost\Games\d3d11-test\d3d11-test.exe /Fo:%TEMP%\ %~dp0main.cpp d3d11.lib dxgi.lib d3dcompiler.lib user32.lib
