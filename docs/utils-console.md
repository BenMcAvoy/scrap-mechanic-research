# `UTILS::Console` reverse-engineering notes

Binary: `ScrapMechanic.exe` 1.0.2.870, IDB session `ed1e9739`.

## Confirmed construction and ownership

The logger construction path is `sub_1402E39F0`, called during `sub_1402E2470` startup. It allocates a `UTILS::Console` through `sub_1402EBA90`, which allocates `0x180` bytes and invokes `sub_1405FEF20`. The constructor installs the `UTILS::Console` vtable, initializes the stream/file-buffer members, stores the singleton in `g_console`, and registers a callback.

The constructor initializes two critical sections. The object keeps its console handle at `+0x50`, an output-code-page flag at `+0x170`, a console-created flag at `+0x171`, and diagnostic counters at `+0x174`/`+0x178`. These member offsets are recorded as reverse-engineering evidence only; the overlay does not use them.

## Vtable

The vtable is `??_7Console@UTILS@@6B@`. The current IDB shows three virtual entries:

| slot | function | observed role |
|---:|---|---|
| 0 | `sub_1405FF1A0` | destructor; emits `Shutting down...`, closes the file buffer, destroys synchronization objects |
| 1 | `sub_1405FF2F0` | synchronized output; applies console color, emits the record, forwards eligible records, updates counters |
| 2 | `sub_1405FF510` | rate-limited/duplicate-suppressed output wrapper; allows one record per two seconds for a key |

The output virtual receives a category/record payload, a source/file pointer, a color (`WORD`), and flags. It enters `stru_141A8D3D8`, writes through `sub_1405FFAA0`, optionally forwards to another sink, restores color, updates counters, and leaves the critical section.

## Console setup

`sub_1402E39F0` replaces the global console object and, when the debug-console condition is enabled, performs the Windows console setup: `AllocConsole`, title `Debug Console`, buffer sizing, `GetStdHandle(STD_OUTPUT_HANDLE)`, default text color, and UTF-8 output code page. Failures are sent through the same logger with the messages `Unable to allocate debug console` and `Unable to setup debug console`.

## Overlay integration

The DLL resolves both diagnostic strings at runtime, finds their containing function, enumerates its relative call sites, and selects the common logger call target whose preceding instructions reference those strings. The resolved wrapper has nine Win64 arguments: category, flags, source path, source line, pointer to source-name pointer, unused pointer-sized slot, pointer to message-line value, unused pointer-sized slot, and message. It dereferences arguments 5, 7, and 9, then forwards the formatted record to the `UTILS::Console` vtable. The overlay now constructs those pointer arguments exactly and calls the game’s logging/console pipeline without embedding a Scrap Mechanic absolute address or private member offset.

If the dynamic resolution fails, the DLL falls back to `OutputDebugStringA`; it does not call an unverified address.

Confidence: high for construction, vtable roles, synchronization, setup, and the nine-argument logger wrapper ABI in this binary. The exact semantic names of the category enum values remain inferred from call sites.

## Logger ABI correction

The first overlay adapter incorrectly declared the resolved wrapper as a five-argument function and passed the message where the native code expects a pointer to source metadata. That caused the game’s `failed to get the log caller` diagnostic. The adapter now passes the source-name pointer, message-line pointer, and message in the native positions; arguments 6 and 8 are explicitly zeroed to preserve Win64 stack layout.

The resolver also now retains a bounded 16-instruction window before each relative call while analyzing the diagnostic setup function. The source-string load is eight instructions before the logger call in this build, so the previous four-instruction window incorrectly reported the logger as missing even though the ABI adapter itself was correct.

## Overlay crash follow-up

The first active overlay build had a separate loader defect: `DllMain` lacked `extern "C"`, so the DLL loaded but did not execute its bootstrap. After correcting that, the renderer initialized successfully. The subsequent game crash occurred after all LuaManager detours were installed and the game reported a Lua deserialization failure for `sv_t_onStayWater`. The current build therefore leaves the Lua detours disabled while retaining the renderer and console diagnostics; they should be re-enabled one at a time after independent signature/lifetime validation.

## Resolver performance follow-up

The slow path was not the function-range lookup or the small console-function disassembly. It was repeated full `.text` scans for each string anchor, plus a `VirtualQuery` call for every candidate RIP-relative instruction. `memorylib` now builds one cached RIP-reference index per module scan, resolves bounded section references without `VirtualQuery`, and caches repeated `.rdata` string searches. The overlay builds successfully with these changes.

## VEH-compatible crash reporting

The game already installs a vectored exception handler. The overlay therefore registers its diagnostic handler with `FirstHandler = FALSE`, after the game has loaded, and always returns `EXCEPTION_CONTINUE_SEARCH`. It does not replace, suppress, or reinterpret the game's handler. Cleanup removes the handler before the DLL is unloaded.

For an exception that survives the game's VEH chain, the overlay writes a text record and a minidump to `%TEMP%` using the pattern `lua_manager_overlay-crash-<pid>-<tick>.(txt|dmp)`. The text record includes exception parameters, x64 registers, and up to 64 frames walked from the saved exception context with `StackWalk64`; the minidump retains thread, module, unloaded-module, and indirectly referenced-memory data. The first validated report was `lua_manager_overlay-crash-32244-108074234.dmp`, with access violation `0xC0000005` at `ntdll+0xB1EB`; this preserves the original exception context without introducing a competing first-chance debugger.
