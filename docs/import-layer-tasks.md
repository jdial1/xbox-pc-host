# Import layer: task list for an LLM coding assistant

## Context (read first)

You are working in the repo `jdial1/xbox-pc-host`: a C++/WinRT UWP app ("host shell") that runs on an
Xbox Series S in Dev Mode and will eventually run unmodified, DRM-free PC games (first target: ClassiCube,
an open-source BSD-licensed D3D11 game, bundled in `XboxPcHost/Games/classicube`).

Facts about the codebase:
- Build: Visual Studio 2022, v143, C++20, x64 only, Windows SDK 10.0.22621, C++/WinRT via NuGet.
  The app is a UWP app container (`WINAPI_FAMILY_APP`): only APIs in the app partition compile.
  Desktop-only APIs (LoadLibrary, CreateProcess, GetModuleFileName, ...) do not exist here.
- `src/App.cpp`: CoreWindow app and shell UI. Pressing A on a game calls `IGameRuntime::Start`.
- `src/GameRuntime.h/.cpp`: `IGameRuntime` + `HostServices`. Currently a stub that only checks PE headers.
- `src/loader/PeImage.h/.cpp`: done. Maps an x64 PE (sections, relocations, protections via
  VirtualProtectFromApp, RtlAddFunctionTable for .pdata). Binds imports by calling
  `ImportResolver::ResolveImport(dll, name, ordinal)`, which must never return null.
- `src/Log.h`: `HOST_LOG(L"fmt", ...)` writes to the debugger and `LocalState\host.log`.
- The manifest declares `codeGeneration`, so executable memory works via VirtualAllocFromApp +
  VirtualProtectFromApp.
- ClassiCube imports: kernel32 (68 functions), user32 (44), gdi32 (7), winmm (8), msvcrt
  (_setjmp, longjmp, memcpy, memmove, memset, strlen), comdlg32 (5), shell32 (2). No static TLS.
  It loads d3d11.dll at runtime via LoadLibrary/GetProcAddress.

Rules:
- Keep the style of the existing code: small classes, `HOST_LOG` for diagnostics, no exceptions
  across the game boundary, comments only where the why isn't obvious.
- Do one task per change. Don't start the next task until the current one builds.
- Never return null from `ResolveImport`; unknown imports get a logging fallback.

## Tasks

1. Resolver class: add `src/loader/Win32Layer.h/.cpp` with a class implementing `ImportResolver`,
   holding a table from `"dll!Name"` to function pointers. Add both files to `XboxPcHost.vcxproj`.
2. Name normalizing: lowercase DLL names, strip `.dll`, map `kernelbase` and `api-ms-win-core-*` to `kernel32`.
3. Logged fallback: for imports not in the table, return a function that logs `dll!Name` on first call
   and returns 0. Log the full list of unresolved imports at load time.
4. Pass-through list: add the kernel32 functions ClassiCube imports that compile in the app partition.
   Anything that doesn't compile stays on the fallback; list them in your report.
5. Wire it up: replace the stub runtime in `GameRuntime.cpp` with one that calls `PeImage::Load`
   with the resolver. Expected log: "Mapped ClassiCube-Win64-D3D11.exe at ..." plus the unresolved list.
6. Run it: start the entry point on its own thread; catch crashes with SEH and log the exception code
   and the faulting offset relative to the image base. Show "running"/"exited"/"crashed" on the shell.
7. Process-level replacements: ExitProcess (record code, end only the game thread),
   GetModuleHandleA/W(NULL) (return the game's image base), GetModuleFileNameA/W (game exe path),
   GetCommandLineA/W (quoted exe path).
8. Test exe: add a tiny console exe under `XboxPcHost/Games/loader-test/` (source in `tools/loader-test/`)
   that prints a line and exits with 42. Expected shell line: "loader-test exited with code 42".

Notes on tasks 1-8:
- Task 3: one shared stub can't tell which import was called. Simple version first: log every
  unresolved name at bind time and point them all at one generic "unimplemented call" stub.
- Task 4: `table["kernel32!Sleep"] = &Sleep;` compiling proves the function is in the app partition.
  msvcrt's memcpy/memmove/memset/strlen can point at the host CRT; _setjmp/longjmp too, because
  the mapper registers .pdata.
- Task 5: the bundled folder is read-only on the console, and ClassiCube writes next to its exe.
  On first launch, copy the game into LocalState\Games and run it from there.

## Later tasks (one sitting each)

9. Files A: CreateFileA/W via CreateFile2FromApp; ReadFile, WriteFile, SetFilePointer, GetFileSize,
   Get/SetFileTime, CloseHandle pass through. One helper maps the game's paths into its LocalState
   folder; A versions convert to W. Done when ClassiCube opens its texture pack zip.
10. Files B: FindFirst/NextFile via FindFirstFileExFromApp; CreateDirectory, DeleteFile, MoveFileEx,
    GetFileAttributes via their FromApp versions; a virtual current directory starting at the game folder.
11. Small fakes: GetVersionExA reports Windows 10; system/windows directory return "C:\Windows";
    SetUnhandledExceptionFilter stores and returns null; CreateProcess, ShellExecute and
    ReadProcessMemory fail with ERROR_ACCESS_DENIED; comdlg32 dialogs return FALSE;
    Global* memory functions go to the heap.
12. Dynamic loading: LoadLibraryA/W return a fake handle per DLL name; GetProcAddress on it goes
    through the resolver and logs every name. Done when the log shows the d3d11 names ClassiCube asks for.
13. Window A: RegisterClass* store the WndProc; CreateWindowEx returns one fake HWND; rect and
    metrics queries report the swapchain size; other window calls return plausible values.
14. Window B: PeekMessage hands out WM_SIZE/WM_ACTIVATE/WM_CLOSE from CoreWindow events;
    DispatchMessage calls the stored WndProc; DefWindowProc returns 0.
15. Graphics: replace D3D11CreateDeviceAndSwapChain to create the swapchain with
    CreateSwapChainForCoreWindow, after the shell releases its own. Done when ClassiCube's first
    frame appears on the TV.
16. Keyboard and mouse messages from Input; GetKeyState; cursor functions.
17. Controller to keyboard/mouse: one mapping table (left stick WASD, right stick mouse,
    A Space, triggers mouse buttons, Menu Esc), per-game later.
18. Audio: the 8 winmm waveOut* functions onto one XAudio2 voice; set WHDR_DONE when a buffer finishes.
19. Leftovers (gdi32, clipboard, shell32) stay stubbed unless the log shows they're needed.

## Report back in this format

After each task, paste this block into the project thread:

```
TASK: <number and name>
COMMIT: <short hash, pushed to main>
BUILD: clean | <first 20 lines of errors>
FILES: <paths changed>
DEVIATIONS: <anything done differently from the task, and why; "none" if none>
STILL STUBBED: <dll!Name list, task 4 onward>
RUNTIME LOG: <relevant host.log lines, tasks 5 onward>
QUESTIONS: <anything blocking>
```
