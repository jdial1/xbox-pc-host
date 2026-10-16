<#
Deploys a built package to an Xbox in Dev Mode through Device Portal, launches
it, and pulls LocalState\host.log back. Works on Windows PowerShell 5.1 and 7.

  $env:XBOX_USER = '<device portal user>'; $env:XBOX_PASS = '<password>'
  .\tools\test\run-test.ps1 -XboxIp 192.168.1.50 -Package .\AppPackages\XboxPcHost_0.1.0.0_x64_Debug.msix

Credentials, if Device Portal asks for them, come from the environment and are handed
to curl on stdin, so they never appear on a command line or in a file. Uses curl.exe (ships with Windows
10+) because Device Portal's certificate is self-signed.

A game only starts when someone presses A in the shell, and this script can't
send controller input. It reports whatever host.log holds after -WaitSeconds.
#>
param(
    [Parameter(Mandatory)] [string] $XboxIp,
    [Parameter(Mandatory)] [string] $Package,
    [string[]] $Dependency = @(),
    [string] $PackageName = 'XboxPcHost',
    [int] $WaitSeconds = 30,
    [string] $OutDir = (Join-Path $PSScriptRoot 'results'),
    [switch] $KeepRunning
)

$ErrorActionPreference = 'Stop'
$haveLogin = [bool]($env:XBOX_USER -and $env:XBOX_PASS) # not needed when Device Portal has authentication turned off
if (-not (Test-Path $Package)) { throw "Package not found: $Package" }
if (-not (Get-Command curl.exe -ErrorAction SilentlyContinue)) { throw 'curl.exe not found.' }

$base = "https://${XboxIp}:11443"
$jar = Join-Path $env:TEMP "wdp-$PID.jar"

function B64([string] $s) { [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($s)) }
function Enc([string] $s) { [Uri]::EscapeDataString($s) }

# Device Portal needs its CSRF token echoed back on every POST/DELETE.
function Csrf {
    foreach ($line in Get-Content $jar -ErrorAction SilentlyContinue) {
        if ($line -match 'CSRF-Token\s+(\S+)\s*$') { return [Uri]::UnescapeDataString($Matches[1]) }
    }
    return ''
}

function Wdp([string] $Method, [string] $Path, [string[]] $Extra = @(), [string] $Out = $null) {
    $curlArgs = @('-sS', '-k', '--fail-with-body', '-b', $jar, '-c', $jar, '-X', $Method)
    if ($haveLogin) { $curlArgs += @('-K', '-') }
    if ($Method -ne 'GET') { $curlArgs += @('-H', "X-CSRF-Token: $(Csrf)") }
    if ($Out) { $curlArgs += @('-o', $Out) }
    $curlArgs += $Extra
    $curlArgs += "$base$Path"
    $result = "user = `"$($env:XBOX_USER):$($env:XBOX_PASS)`"" | & curl.exe @args
    if ($LASTEXITCODE -ne 0) { throw "Device Portal $Method $Path failed (curl $LASTEXITCODE): $result" }
    return $result
}

function InstalledPackage {
    $json = Wdp GET '/api/app/packagemanager/packages' | ConvertFrom-Json
    $json.InstalledPackages | Where-Object { $_.Name -eq $PackageName } | Select-Object -First 1
}

New-Item -ItemType Directory -Force $OutDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'

try {
    Wdp GET '/api/os/info' | Out-Null   # primes the CSRF cookie and proves the credentials work
    Write-Host "Connected to $XboxIp"

    $old = InstalledPackage
    if ($old) {
        Write-Host "Uninstalling $($old.PackageFullName)"
        Wdp DELETE "/api/app/packagemanager/package?package=$(Enc $old.PackageFullName)" | Out-Null
    }

    Write-Host "Installing $(Split-Path $Package -Leaf)"
    $form = @('-F', "file=@$Package")
    foreach ($d in $Dependency) { $form += @('-F', "file=@$d") }
    Wdp POST "/api/app/packagemanager/package?package=$(Enc (Split-Path $Package -Leaf))" $form | Out-Null

    $deadline = (Get-Date).AddSeconds(120)
    do {
        Start-Sleep -Seconds 3
        $pkg = InstalledPackage
    } until ($pkg -or (Get-Date) -gt $deadline)
    if (-not $pkg) { throw 'Install did not finish within 120 s.' }

    Write-Host "Launching $($pkg.PackageRelativeId)"
    Wdp POST "/api/taskmanager/app?appid=$(Enc (B64 $pkg.PackageRelativeId))&package=$(Enc (B64 $pkg.PackageFullName))" | Out-Null

    Write-Host "Waiting $WaitSeconds s"
    Start-Sleep -Seconds $WaitSeconds

    $log = Join-Path $OutDir "host-$stamp.log"
    Wdp GET "/api/filesystem/apps/file?knownfolderid=LocalAppData&filename=host.log&packagefullname=$(Enc $pkg.PackageFullName)&path=%5C%5CLocalState" -Out $log | Out-Null
    Write-Host "Saved $log"

    if (-not $KeepRunning) {
        try { Wdp DELETE "/api/taskmanager/app?package=$(Enc (B64 $pkg.PackageFullName))&forcestop=true" | Out-Null }
        catch { Write-Warning "Couldn't stop the app: $_" }
    }
}
finally {
    Remove-Item $jar -ErrorAction SilentlyContinue
}

# Get-Content decodes by BOM, so this works whether host.log is UTF-8 or UTF-16.
$text = Get-Content $log -Raw
$patterns = 'Mapped ', 'unresolved:', 'pass-through:', 'game thread started', 'exited with code', 'crashed:', 'ExitProcess\(', 'unimplemented call:'
$lines = $text -split "`r?`n" | Where-Object { $l = $_; $patterns | Where-Object { $l -match $_ } }
Write-Host "`n--- relevant host.log lines ---"
$lines | ForEach-Object { Write-Host $_ }

if ($text -match 'crashed:') { Write-Host "`nRESULT: crashed"; exit 2 }
if (-not $lines) { Write-Host "`nRESULT: no loader lines (was A pressed on a game?)"; exit 3 }
Write-Host "`nRESULT: ok"
