# No Smart Physics

An x64 Scrap Mechanic DLL that disables the Advanced-to-Smart physics fallback at runtime.

The DLL uses the sibling `memorylib` project to resolve the target dynamically:

1. Find the unique `Physics hung!` string and its RIP-relative code reference.
2. Resolve the containing `GameInstance` function.
3. Scan that function for `cmp r15d, 8; jnz ...`.
4. Replace only that conditional branch with an unconditional jump to its existing destination.

The DLL therefore does not contain a game RVA or absolute address.

The resolved branch corresponds to the `GameInstance.cpp:581` Advanced-physics fallback identified during analysis, but no build-specific address is embedded in the DLL.

The original executable file and IDA database are not modified. The live game process is temporarily made writable for the six-byte patch, then its original protection is restored.

## Build

```powershell
xmake f -p windows -a x64 -m release
xmake build no_smart_physics
```

The output is `build/windows/x64/release/no_smart_physics.dll` (or the matching debug directory).

Inject the DLL using the loader of your choice before or during game startup. The DLL installs itself from `DllMain`; the exported `NoSmartPhysics_Install` and `NoSmartPhysics_Remove` functions are also available for controlled loaders.
