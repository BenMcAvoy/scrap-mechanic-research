---
name: scrap-mechanic-trace-serialization
description: Reverse engineer Scrap Mechanic serialization of Lua values, native handles, UUIDs, objects, save data, and network payloads with IDA Pro MCP. Use when tracing how a value or object is encoded, decoded, stored, or transmitted.
---

# Scrap Mechanic Trace Serialization

## Purpose

Recover the path and provisional layout used to serialize Scrap Mechanic values and objects without pretending that an inferred format is proven.

## Verified Scrap Mechanic Anchors

The current binary contains `LuaObjectSerializer.h`, `LuaObjectSerializer.cpp`, `LUA_MAGIC_TAG`, diagnostics for invalid serialized Lua data and serialization versions, `LuaTerrainGameEnvPtr`, `UuidNetworkMap.cpp`, `g_contraptionUuidNetworkMap`, and network wrapper code. These provide concrete entry points for Lua, UUID, and network serialization investigations.

For exact current-IDB candidates and observed call patterns, read [references/current-idb-scenarios.md](references/current-idb-scenarios.md).

## MCP Sequence

1. Call `idb_list` and select the active IDA session.
2. Use `find_regex` for `LuaObjectSerializer`, `LUA_MAGIC_TAG`, version diagnostics, UUID-map strings, or source paths.
3. Use `xrefs_to` and `decompile` to identify paired encode/decode candidates.
4. Use `func_profile` to compare callers, callees, constants, and buffer-related strings; use `trace_data_flow` on the buffer or handle.
5. Apply provisional types/structures only after comparing both directions and multiple callers; do not patch bytes.

## Workflow

1. Start from a distinctive type name, serializer error, magic tag, version string, UUID operation, save/network call, or known serialize/deserialize caller.
2. Search strings and imports, then cross-reference the candidate functions. Prioritize paired `Serialize`/`Deserialize` paths and functions that consume a buffer plus a cursor/size.
3. Decompile and mark reads/writes by category: magic/version, type tag, length, UUID, numeric value, string, object handle, nested container, and terminator.
4. Trace data flow backward from decoded fields and forward from encoded fields. Compare the encode and decode paths for symmetry.
5. Separate Lua serialization from native/network/save serialization. A common helper may be shared, but the surrounding framing and ownership can differ.
6. Validate inferred offsets and field types against multiple call sites, version checks, error strings, and size calculations.

## Concrete Binary Scenario: LuaObjectSerializer

Use the current IDB's `LuaObjectSerializer` cluster:

- `sub_140245780` references the diagnostic `LuaObjectSerializer::Deserialize was given data that is not serlized Lua data.` and is called from two higher-level functions;
- `sub_14071D6A0` and `sub_14071D970` both reference `LuaObjectSerializer.cpp`, and the former calls the latter;
- `sub_14071D970` also references `JsonTemplate did not find any dynamic template under: {`.

Decompile all three, identify direction from buffer reads/writes and callers, then compare their type-tag, length, and nested-value handling. Do not name a routine `Serialize` or `Deserialize` solely from its source path or neighboring function; prove the direction from data flow.

## Scrap Mechanic Anchors

Look for `LuaObjectSerializer`, Lua type tags, magic/version diagnostics, UUID strings/helpers, `lua_newuserdata`, object-handle conversion, and manager/contraption serialization callers. Treat source paths and error text as anchors, not proof of format.

## Metadata and Output

Create provisional structures and enums only when evidence supports them; label uncertain fields. Add comments with byte order/size evidence and version assumptions. Never patch bytes without explicit confirmation.

```text
Value/object:
Serialize function:
Deserialize function:
Framing/magic/version:
Field sequence:
Handle/UUID behavior:
Callers and context:
Confidence and unknowns:
```
