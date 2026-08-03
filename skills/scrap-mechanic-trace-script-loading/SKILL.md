---
name: scrap-mechanic-trace-script-loading
description: Trace Scrap Mechanic Lua script loading, environment creation, execution, and callback setup through IDA Pro MCP. Use for script paths under GAME_DATA, SURVIVAL_DATA, CHALLENGE_DATA, or mod templates.
---

# Scrap Mechanic Trace Script Loading

## Purpose

Follow a Scrap Mechanic Lua script path from its embedded string or loader call through path resolution, file/buffer loading, Lua environment setup, execution, and registration of callbacks or globals.

## Verified Scrap Mechanic Anchors

The current binary contains `LuaManager.h`, `LuaManager.cpp`, `Initializing LuaManager as client`, `Initializing LuaManager as server`, `Cleaning up LuaManager and Interfaces`, Lua calls including `luaL_loadbufferx`, `luaL_loadstring`, `lua_pcall`, `lua_call`, and `lua_setfenv`, plus script roots such as `$GAME_DATA/Scripts/game`, `$SURVIVAL_DATA/Scripts/game`, `$CHALLENGE_DATA/Scripts`, and ExampleMods template scripts.

For exact current-IDB candidates and observed call patterns, read [references/current-idb-scenarios.md](references/current-idb-scenarios.md).

## MCP Sequence

1. Call `idb_list` and identify the active session.
2. Use `find_regex` for an exact script path or LuaManager initialization string.
3. Use `xrefs_to` to find initialization/load functions and `search_text` scoped to their code ranges for `luaL_load*`, `lua_pcall`, or `lua_call`.
4. Decompile the loader and use `func_profile` for script-path strings, environment helpers, and callers.
5. Trace the loaded script into callback/API registration; annotate client/server/shared role and preserve path roots.

## Workflow

1. Search for the exact script path or a distinctive filename such as `CreativeGame.lua`, `SurvivalGame.lua`, or a mod template path.
2. Cross-reference every occurrence and distinguish registration tables, diagnostics, and actual loader calls.
3. Decompile loader candidates and identify path normalization, data-root selection, file/buffer reads, `luaL_load*`, `lua_pcall`/protected calls, and error handling.
4. Trace the Lua state/environment used for the script. Record global tables, sandbox setup, client/server role, mod namespace, and injected native APIs.
5. Follow post-load calls to callback registration, class/table construction, or manager initialization.
6. Compare the same script family across Creative, Survival, Challenge, client, server, and mod-template paths.

## Concrete Binary Scenario: Game Script Initialization

Use `sub_140366BC0` as the LuaManager initialization anchor. It references `Initializing LuaManager as client`, `Initializing LuaManager as server`, and `GameInstance.cpp`. Then inspect `sub_14036B2E0`, which references `SurvivalGame.lua`, `MenuGame.lua`, `CreativeGame.lua`, and the `seed`/`dev` configuration keys. Follow its callers and callees until the `luaL_load*`/protected-call boundary is reached. This gives a concrete comparison of shared setup versus mode-specific script loading.

## Evidence Rules

Do not equate a path string with execution. Prove execution by finding the loader/protected-call sequence and connect it to the requested environment. Preserve exact path roots and role distinctions because they are useful anchors across versions.

## Metadata and Output

Rename confirmed loader phases, add comments with script path and environment, and apply types to loader/context objects when supported by multiple callers. Never modify executable bytes without explicit confirmation.

```text
Script path:
Loader/initializer:
Lua state/environment:
Role: client | server | shared | unknown
Execution call:
Callback/API registration:
Evidence and confidence:
```
