# Builds the TLS slot DLLs (see TlsSlot.cpp) into the given folder: XboxPcHostTls1-2.dll with 64 KB
# blocks and XboxPcHostTls3-10.dll with 4 KB ones. Needs a VS developer shell (vcvars64).
param([Parameter(Mandatory)] [string] $Out)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $Out | Out-Null
$obj = Join-Path $Out 'obj'
New-Item -ItemType Directory -Force $obj | Out-Null
foreach ($size in 65536, 4096) {
    $o = Join-Path $obj "TlsSlot$size.obj"
    cl /nologo /c /O1 /MT /DTLS_SLOT_BYTES=$size /Fo"$o" "$PSScriptRoot\TlsSlot.cpp"
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    link /nologo /DLL /APPCONTAINER /OPT:REF /OUT:"$obj\TlsSlot$size.dll" "$o"
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
foreach ($i in 1..10) {
    $size = if ($i -le 2) { 65536 } else { 4096 }
    Copy-Item -Force "$obj\TlsSlot$size.dll" (Join-Path $Out "XboxPcHostTls$i.dll")
}
