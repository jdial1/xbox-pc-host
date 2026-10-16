# Local testing

Build on a Windows machine with Visual Studio 2022 (UWP C++ workload, SDK 10.0.22621), then
deploy to a console in Dev Mode with `tools/test/run-test.ps1`.

```
msbuild XboxPcHost.sln /restore /p:RestorePackagesConfig=true /p:Configuration=Debug /p:Platform=x64 /m
```

Create an app package (Project > Publish > Create App Packages, sideloading, unsigned or with a
test certificate), then:

```
$env:XBOX_USER = '<device portal user>'
$env:XBOX_PASS = '<password>'
.\tools\test\run-test.ps1 -XboxIp 192.168.1.50 -Package .\AppPackages\XboxPcHost_0.1.0.0_x64_Debug_Test\XboxPcHost_0.1.0.0_x64_Debug.msix
```

The script uninstalls the old copy, installs, launches, waits `-WaitSeconds` (30), downloads
`LocalState\host.log` into `tools/test/results/`, prints the loader lines, and stops the app.
Exit code 0 is ok, 2 is `crashed:`, 3 is no loader lines in the log.

## Limits

- Pair the console with Visual Studio once in Dev Home and turn on Device Portal. The console has
  to be reachable on port 11443.
- The script can't press A. A game only starts when someone selects it in the shell, so until
  the app has an auto-launch switch for test builds, press A by hand during the wait and use
  `-WaitSeconds 60` to leave time. Without it the log only shows the startup lines.
- The Device Portal calls in the script have not been run against a console yet. Treat the first run
  as a check of the script too, and compare against `https://<ip>:11443/` if a call fails.
- Credentials are read from `XBOX_USER` and `XBOX_PASS` and passed to curl on stdin. Don't commit
  `tools/test/results/`.

## Without a console

`cl`-only compile check (what the first eight import-layer tasks used when the UWP workload
was missing) catches compile and link errors but runs nothing. Only the console exercises the
loader.
