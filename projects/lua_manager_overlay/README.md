# LuaManager overlay and structure viewer

The DLL is now an internal ImGui LuaManager structure viewer. It installs only the D3D11 Present/resize integration and the game-window message integration required to render and capture input; all LuaManager/function detours are removed from the active runtime. It resolves the version-specific `LuaManager` singleton directly and reads the structure from the render path with guarded reads.

Current-IDB evidence identifies the singleton as `qword_141AA25A8`, assigned by the constructor at `0x14082E130`. For this binary the slot RVA is `0x1AA25A8`; the role byte is at `LuaManager+0x354`, and the `LuaVM` shared-pointer object/control-block pair is at `+0x358/+0x360`. These addresses are version-specific and are kept explicit in `src/viewer.cpp`.

The DLL remains an optional D3D11 overlay for visual diagnostics. LuaManager detours and live structure capture are disabled in the stable default configuration.

The overlay hooks the DXGI/D3D11 swap-chain `Present` path, which keeps it compatible with the Steam overlay layer. It observes these LuaManager paths when their symbols can be resolved:

- LuaManager initialization
- mode script selection/loading
- client update dispatch
- fixed-update dispatch
- VM refresh/class rebuild
- client-data update dispatch
- lifecycle dispatch (create/destroy/refresh callback phases)

It displays resolved function addresses, hook status, call counts, last `this` pointer, last thread, the raw mode scalar and address, client-detour phase, lifecycle arguments, and the currently selected script path/class inferred from the mode loader's string anchors. It also displays guarded raw LuaManager fields recovered from the current IDB: client/server role, callback/reentrancy state, transient callback context, callback counters/cursors, the five callback-vector begin/end/capacity triplets, guarded samples of callback-object fields at `+0x10/+0x18/+0x20/+0x24/+0x28/+0x30/+0x38`, lifecycle/script containers, registry/hash-table storage, and the LuaVM shared-pointer pair. Unknown field semantics remain labeled by offset rather than guessed; a scrollable raw qword snapshot of `LuaManager+0x000..0x367` keeps the remaining version-specific state visible. It never crosses the game boundary with `std::string`, Lua, or STL objects, and does not dereference those private pointers.

Build from this directory:

```powershell
xmake f -p windows -a x64 -m release
xmake build -y lua_manager_overlay
xmake build -y lua_manager_structure_viewer
```

The DLL is `build/windows/x64/release/lua_manager_overlay.dll`.
The standalone external viewer remains available as a read-only fallback at `build/windows/x64/release/lua_manager_structure_viewer.exe`.

Run the internal DLL through the project injector. The standalone viewer can still be run independently:

```powershell
Start-Process .\build\windows\x64\release\lua_manager_structure_viewer.exe
```

Use Refresh for a manual snapshot or enable Auto refresh for one-second updates. The viewer only reads memory and does not write to the game.
