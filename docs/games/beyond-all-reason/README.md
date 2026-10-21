# Beyond All Reason (Recoil engine)

Free and open source (GPL engine, open game content). Status: the headless engine plays an AI-vs-AI
skirmish in real time on the Xbox (host 0.1.18.0); graphics are in progress (Mesa's d3d12 OpenGL driver).

## Getting the files (on a PC)
1. Engine: `recoil_<version>_amd64-windows.7z` from github.com/beyond-all-reason/RecoilEngine/releases,
   extracted into the game folder.
2. Game and maps with the engine's `pr-downloader.exe`, pointed at BAR's servers:
   ```
   set PRD_RAPID_USE_STREAMER=false
   set PRD_RAPID_REPO_MASTER=https://repos-cdn.beyondallreason.dev/repos.gz
   set PRD_HTTP_SEARCH_URL=https://files-cdn.beyondallreason.dev/find
   pr-downloader --filesystem-writepath . --download-game byar:test
   pr-downloader --filesystem-writepath . --download-map "Red Comet Remake 1.8"
   ```
   `host-skirmish.txt` names the game version by its full name (`rapid\...\versions.gz` maps
   `byar:test` to it); update `GameType` after a new download.
3. `msvcrt.dll` from `C:\Windows\System32` into the game folder: the MinGW build needs the real one, and
   the host gives a game's own copy all of its msvcrt imports.
4. The files here: `host-exe.txt`, `host-args.txt`, `host-skirmish.txt` (two BARb AIs, the player
   spectates), and the lines of `springsettings-console.txt` appended to `springsettings.cfg`.

## Memory
The console gives the app 5 GB. Headless BAR commits 6.2 GB on a large map with default settings; with
`springsettings-console.txt` and a small map it is 3.5 GB. Use `--isolation` without `--write-dir`: a
relative write dir gets its "." glued onto file names.

## OpenGL through Mesa
Mesa (mesa-dist-win, `GALLIUM_DRIVER=d3d12`) gives Recoil OpenGL 4.6 Compatibility and renders it
correctly. Recoil's Lua VAO draws use primitive-restart index 0xFFFFFF for 32-bit indices, which D3D12
can't do natively, so Mesa rewrites every such draw on the CPU after a GPU sync (0.8 fps). Restart index
0xFFFFFFFF behaves the same for these buffers and is native: `find_restart.py` patches a copy of
spring.exe that way for PC tests (27 fps on a GTX 1050 Ti). `host_probe.lua` (LuaUI/Widgets) logs fps
and takes screenshots for runs without a desktop.
