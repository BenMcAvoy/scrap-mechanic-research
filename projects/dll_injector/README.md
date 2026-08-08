# DllInjector

Self-contained x64 .NET injector for the local Scrap Mechanic research project.

```powershell
.\DllInjector.exe inject <pid> <absolute-dll-path> [timeout-ms]
.\DllInjector.exe uninject <pid> <module-name-or-absolute-dll-path> [timeout-ms]
```

The published executable is in `bin/Release/net8.0-windows/win-x64/publish/DllInjector.exe`.

The overlay writes diagnostics to `%TEMP%\lua_manager_overlay.log`. Always inspect that file before and after injection. `uninject` calls the DLL's exported `LuaManagerOverlay_Unload` entry point, waits for its cleanup thread, and verifies that the module can be removed without terminating the game.

For the current overlay build:

```powershell
$injector = '.\DllInjector.exe'
$dll = 'C:\Users\Ben\scrap_research\projects\lua_manager_overlay\build_diag\windows\x64\release\lua_manager_overlay.dll'
& $injector inject <pid> $dll
Get-Content "$env:TEMP\lua_manager_overlay.log"
& $injector uninject <pid> lua_manager_overlay.dll
```
