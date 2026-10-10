# Xbox PC Host: host shell (stage 1)

A plain C++/WinRT UWP app for Xbox Series S in Dev Mode. No XAML: it runs on a
CoreWindow and owns the pieces later stages plug into.

| Piece | File | What it does now |
|---|---|---|
| Window and lifecycle | `src/App.cpp` | CoreWindow app, game loop, suspend/resume, swallows the B-button "back" so the app doesn't close |
| Swapchain | `src/Graphics.*` | D3D11 device, flip-model swapchain on the CoreWindow, resize and device-lost handling, Direct2D text for the status screen |
| Input | `src/Input.*` | First connected controller (with dead zones and button-press edges), plus USB keyboard and mouse |
| Audio | `src/Audio.*` | XAudio2 engine with one 48 kHz stereo stream (`Submit`) and a test tone |
| Game folder | `src/GameLibrary.*` | Lists games from `<package>\Games\` (bundled, read-only) and `LocalState\Games\` (copied on, writable) |
| Plug-in seam | `src/GameRuntime.*` | `IGameRuntime` + `HostServices`. The stub checks the exe's PE headers and reports that the loader isn't built yet |
| Log | `src/Log.*` | Debugger output plus `LocalState\host.log` |

ClassiCube (D3D11, x64) is already in `XboxPcHost/Games/classicube/`, so it is
bundled into the package and shows up in the list on first launch.

## Controls

D-pad up/down selects a game, A launches it (for now this only inspects the
exe and shows the result), Y rescans the folders, X plays a test tone. The
left stick and triggers tint the background so you can see input working.

## Build and deploy

You need Windows 10/11 with Visual Studio 2022 and these workloads:
"Universal Windows Platform development" with the "C++ (v143) Universal Windows
Platform tools" component, and a Windows 11 SDK (10.0.22621).

1. Put the Xbox in Dev Mode and open Dev Home. Note the console's IP address and
   turn on Remote Access / Device Portal.
2. Open `XboxPcHost.sln`. Visual Studio restores the C++/WinRT NuGet package on
   first build (if it doesn't, right-click the solution > Restore NuGet Packages).
3. Set the configuration to `Debug | x64` and the target to **Remote Machine**.
   In Project > Properties > Debugging, set Machine Name to the console's IP and
   Authentication Type to **Universal (Unencrypted Protocol)**.
4. Press F5. The first time, Visual Studio asks for a PIN: in Dev Home choose
   "Pair with Visual Studio" to get one.
5. Set the app to run as a Game, not an App, or it is limited to a small slice
   of memory and GPU: in Dev Home, highlight Xbox PC Host, press the View
   button > View details, and change App type to **Game**. Restart the app.

You should see the status screen with your GPU name, the output resolution,
live controller values, "XAudio2 48 kHz stereo", and `classicube` in the list.
Press A on it: the message line should say it's a valid x64 PE and the loader
isn't built yet.

### Without Visual Studio deploying directly

Project > Publish > Create App Packages > Sideloading, let it create a test
certificate, then in Device Portal (`https://<xbox-ip>:11443`) go to My games &
apps > Add and upload the `.msix` plus the dependency packages from the
`Dependencies\x64` folder.

### Adding more games

Either drop a game folder into `XboxPcHost/Games/` before building, or use
Device Portal > File explorer > LocalAppData > XboxPcHost_... > LocalState >
Games and upload it there, then press Y. `host.log` sits in that LocalState
folder too.

## Not checked yet

Written without a Windows machine. Every source file compiles cleanly (syntax
and types) with clang against the real Windows 10.0.22621 SDK and C++/WinRT
headers, but it has not been linked by MSVC or run on a console yet. If the
first Visual Studio build or launch shows errors, paste them back.

## Next: stage 2 (loader)

Implement a real `IGameRuntime` in `GameRuntime.cpp`: map the exe's sections
with `VirtualAllocFromApp`, apply relocations, resolve imports against the
stage-3 call layer, set protections with `VirtualProtectFromApp` (allowed by the
`codeGeneration` capability in the manifest), and jump to the entry point on
its own thread.
