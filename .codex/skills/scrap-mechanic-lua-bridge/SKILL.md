---
name: scrap-mechanic-lua-bridge
description: Trace Scrap Mechanic native objects through the Lua bridge using IDA Pro MCP. Use when analyzing userdata wrappers, metatables, native type checks, Lua object handles, C++ to Lua conversion, or how a manager/class becomes a Lua object.
---

# Scrap Mechanic Lua Bridge

## Purpose

Map the boundary between Scrap Mechanic C++ objects and Lua values. Use this when the native callback is known but the object wrapper, userdata layout, metatable, type check, or lifetime behavior is not.

## Verified Scrap Mechanic Anchors

The current binary contains `lua_newuserdata`, `lua_setmetatable`, and `luaL_checkudata` imports, plus bridge source markers such as `LuaVM.h`, `LuaCallbacks.h`, `LuaObjectSerializer.h`, and `LuaTerrainGameEnvPtr`. Use these as concrete starting points, then follow their xrefs rather than assuming every Lua API call belongs to the same wrapper.

For exact current-IDB candidates and observed call patterns, read [references/current-idb-scenarios.md](references/current-idb-scenarios.md).

## MCP Sequence

1. Call `idb_list` and use the active session ID for `database`.
2. Use `find_regex` for a metatable/type/error anchor or `search_text` for `lua_newuserdata`, `luaL_checkudata`, and `lua_setmetatable` in `.text`.
3. Use `xrefs_to` on the anchor, then `decompile` each candidate wrapper.
4. Use `func_profile` to list callers, callees, strings, and constants; use `trace_data_flow` for the native handle.
5. Use `rename` with `dry_run: true` before applying metadata names; never use byte-writing tools.

## Workflow

1. Establish the active IDA database session and use read-only MCP discovery tools.
2. Start from a known Lua callback, metatable name, type name, `lua_newuserdata`, `lua_setmetatable`, `luaL_checkudata`, `lua_getfield`, registry access, or a distinctive wrapper error string.
3. Decompile candidate functions and classify every Lua API operation: stack read, type check, userdata allocation, metatable lookup, native-handle extraction, result conversion, and cleanup.
4. Follow the helper that extracts stack index 1. Determine whether it checks a metatable, reads a handle/index, validates a vtable, or unwraps a shared pointer.
5. Follow the metatable creation/registration path. Record the Lua-visible type name, method table, `__index` behavior, destructor/finalizer, and native class association.
6. Compare multiple callbacks using the same wrapper helper to identify the common bridge convention.

## Concrete Binary Scenarios

### Network userdata and metatable family

In the current IDB, inspect the cluster around `sub_140A66F90` through `sub_140A679D0`. The disassembly shows repeated `lua_newuserdata`, `lua_getfield`, `lua_setmetatable`, and `luaL_checkudata` patterns. Use it to recover:

- which native object each wrapper represents;
- the Lua metatable field used for construction;
- the `luaL_checkudata` type-check name;
- the native pointer/handle stored in userdata;
- whether the wrapper is a value, borrowed pointer, or owned object.

Treat adjacent functions as a family only after comparing their type strings and native calls.

### `Character:getTpBonePos` return wrapper

The known callback at `sub_1406425A0` extracts a Character receiver, calls the Character manager, allocates 12 bytes with `lua_newuserdata`, writes three components, and applies the `Vec3` metatable. Use it as a compact example of native lookup followed by Lua value conversion. The method skill identifies the registration; this skill should explain the returned userdata and metatable path.

## Evidence Rules

- Do not infer a native class from a Lua name alone; prove it through metatable/type checks, vtable use, field offsets, or manager calls.
- Distinguish a raw pointer, object ID, pool index, shared pointer, and serialized handle. Their ownership and lifetime behavior differ.
- Treat userdata size and field offsets as provisional until corroborated by allocation, extraction, and destruction paths.
- Use `trace_data_flow` for handles and `func_profile` for wrapper helpers; use `callgraph` only for call relationships.

## Metadata

Rename confirmed bridge helpers and locals, apply evidence-based signatures and wrapper structures, and add comments describing Lua type, userdata layout, ownership, and native class. Never patch bytes without explicit confirmation; metadata edits are encouraged.

## Report

```text
Lua type:
Native class:
Metatable/registration:
Wrapper helper:
Userdata layout:
Ownership/lifetime:
Evidence:
Confidence:
Unresolved assumptions:
```
