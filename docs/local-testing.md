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
- If Device Portal asks for a login, set `XBOX_USER` and `XBOX_PASS`; they are passed to curl on stdin. With authentication off, leave them unset. Don't commit
  `tools/test/results/`.

## Without a console

`cl`-only compile check (what the first eight import-layer tasks used when the UWP workload
was missing) catches compile and link errors but runs nothing. Only the console exercises the
loader.

## Desktop harness (no console, works over SSH)

`tools/host-test` builds `host-test.exe`, which runs the app's own `PeImage`, `Win32Layer` and
runtime code as a plain desktop console program. From a VS developer shell:

```
.\tools\host-test\build.ps1
.\tools\host-test\out\host-test.exe XboxPcHost\Games\loader-test\loader-test.exe --expect "exited with code 42"
```

It prints the runtime's status line and writes `host.log` next to the exe. Exit code 0 is ok, 1 is a
load failure or an unexpected end, 2 means `--expect` wasn't found, 3 is a timeout.

What it proves: mapping, relocations, import binding, the unresolved list, the game thread, SEH crash
reporting, and the process shims. What it can't: app container rules (VirtualProtectFromApp limits,
the `codeGeneration` capability, blocked APIs) and Direct3D output. Those need the console, or
the MSIX installed on a Windows 10/11 client PC.

Games that draw through GDI (OpenTTD, gdi-test, ClassiCube's launcher) can be watched and driven:

- `--shot file.bmp` saves the last software frame when the run ends.
- `--script file` runs timed steps, one per line, in seconds from the game's start:
  `<s> move <x> <y>`, `<s> click [right]`, `<s> key <vk>` (decimal or `0x` hex), `<s> shot <file.bmp>`,
  `<s> close`. `#` starts a comment.

For example, this gets OpenTTD from its title screen into a new game at 1920x1080 (positions are
window pixels; `host-args.txt` should pass `-r 1920x1080`):

```
8.5  move 663 484     # close the "fallback sound set" message
9    click
10.5 move 813 618     # "No" to the survey
11   click
12.5 move 958 267     # New Game
13   click
15.5 move 958 800     # Generate
16   click
25   shot ingame.bmp
```

## What the harness shows for ClassiCube

`XboxPcHost/Games/classicube/host-args.txt` holds `--singleplayer`, which the loader appends to the
command line so the game skips its GDI launcher and goes straight to Direct3D. On the desktop the
log should reach `D3D11CreateDevice -> 0x00000000`, then `game swapchain failed` because there is
no CoreWindow. On the console it should print `game swapchain <w>x<h> created on CoreWindow` and
`first game frame presented`.

After the game takes the CoreWindow the shell can't draw again; restart the app to launch another
game. The shell's swapchain isn't recreated when the game exits.

## Test programs

Small freestanding exes under `XboxPcHost/Games/` (sources and `build.cmd` in `tools/`), run with `host-test.exe <exe> --expect "exited with code N"`:

| Exe | What it checks | Expected |
|---|---|---|
| `loader-test` | mapping, imports, `ExitProcess` | code 42 |
| `window-test` | window class, `WM_SIZE`/`WM_ACTIVATE`, keyboard, mouse, wheel | code 127 with `--inject keys` or `--inject pad` |
| `gdi-test` | DIB section, memory DC, `BitBlt`, `SetDIBitsToDevice`, `GetDeviceCaps` | code 127 |
| `audio-test` | `waveOut` open, play, `WHDR_DONE`, close | code 31 on a machine with an audio device (code 0 when there is none) |

`--inject pad` drives the controller mapping in `src/InputMapping.cpp`; `--inject keys` drives a real keyboard and mouse.

## ClassiCube's launcher

Delete (or rename) `XboxPcHost/Games/classicube/host-args.txt` to start the launcher instead of
singleplayer. It draws a 1920x1080 software bitmap through gdi32; the log shows
`CreateDIBSection 1920x1080 top-down` and `first GDI frame 1920x1080`. On the console each
`BitBlt` sends that bitmap to the screen; the harness has no screen, so it only logs the first frame.
Its network calls go to the real Winsock, and the certificate checks for TLS to the real crypt32, so
the launcher's update check works: the log shows `HTTP: result 0 (http 200)` for
`cdn.classicube.net/client/builds.json`. That means running it on a test machine makes real
requests to ClassiCube's servers.

A game's worker threads can outlive its main thread, and the app partition has no `TerminateThread`.
When that happens the log says `N game thread(s) still running; keeping the image mapped` and the
game's image stays mapped instead of being unmapped under those threads.
