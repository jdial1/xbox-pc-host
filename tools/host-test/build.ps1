# Builds tools\host-test\out\host-test.exe from the app's loader sources. Needs the MSVC
# and Windows SDK paths from a VS developer shell (VsDevCmd -arch=x64).
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\XboxPcHost')
$out = Join-Path $PSScriptRoot 'out'
New-Item -ItemType Directory -Force $out | Out-Null
$src = @("$root\src\GameRuntime.cpp", "$root\src\Log.cpp") + (Get-ChildItem "$root\src\loader\*.cpp").FullName
cl /nologo /std:c++20 /EHsc /MD /DUNICODE /D_UNICODE /I"$root\src" /Fo"$out\" /Fe"$out\host-test.exe" "$PSScriptRoot\main.cpp" $src WindowsApp.lib
exit $LASTEXITCODE
