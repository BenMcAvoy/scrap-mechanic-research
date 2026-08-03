---
name: scrap-mechanic-trace-events
description: Trace Scrap Mechanic update loops, event registration, callbacks, and client/server dispatch through IDA Pro MCP. Use when locating where a callback is registered, scheduled, or invoked.
---

# Scrap Mechanic Trace Events

## Purpose

Map a Scrap Mechanic event from its registration site to storage, scheduling/update code, dispatch, and the final callback or Lua/native receiver.

## Verified Scrap Mechanic Anchors

The current binary contains `LuaCallbacks.h`, `LuaCallbacks.cpp`, `Client2ServerEvents.cpp`, `Contraption_event.cpp`, `sm.event`, `bindEventCallback`, `bindEventClientCallback`, `bindEventServerCallback`, `clearEventCallbacks`, `client_onUpdate`, `client_onFixedUpdate`, `server_onReceiveUpdate`, `server_onFixedUpdate`, and `Lua call buffer` callback diagnostics. These are stronger anchors than generic `event` strings, many of which are FMOD audio event names.

For exact current-IDB candidates and observed call patterns, read [references/current-idb-scenarios.md](references/current-idb-scenarios.md).

## MCP Sequence

1. Call `idb_list` and use the active session ID.
2. Use `find_regex` for a gameplay callback such as `client_onUpdate`, `server_onFixedUpdate`, `bindEventServerCallback`, or a Lua call-buffer diagnostic.
3. Use `xrefs_to` and `decompile` the referencing callback guard, registration function, or dispatcher.
4. Use `func_profile` for callers/callees and `trace_data_flow` for callback handles, script IDs, or event tables.
5. Confirm an actual registration or call edge before renaming; avoid FMOD `event:/...` strings unless audio dispatch is the target.

## Workflow

1. Start from a callback name, Lua method, log/error string, manager method, update/tick clue, or known function pointer. Prefer gameplay callback names and `sm.event`/`bindEvent*`; do not start from an `event:/...` FMOD audio path unless audio dispatch is the actual target.
2. Search strings and xrefs, then identify whether each hit is a registration entry, callback implementation, diagnostic, or invocation.
3. Inspect tables/arrays of callback pointers and adjacent metadata. Record event name, owner, phase, priority/order, and client/server role.
4. Trace the stored callback into update loops, queues, task systems, or manager dispatchers with `xrefs_to`, `callgraph`, `callees`, and `trace_data_flow`.
5. Decompile the invocation path and determine argument construction, lifetime, reentrancy/guard flags, and error handling.
6. Compare native and Lua callbacks, and compare client/server event paths when both exist.

## Concrete Binary Scenario: Script Callback Dispatch

Use the LuaManager callback cluster in the current IDB:

- `sub_14082F5E0` references the `server_onFixedUpdate` and `client_onFixedUpdate` reentrancy diagnostics;
- `sub_140830020` references the `client_onUpdate` reentrancy diagnostic;
- `sub_14083B3B0` references `Lua call buffer - failed to call callback`;
- all three are reached from shared higher-level LuaManager code.

Compare these functions to identify common callback invocation helpers, client/server callback selection, reentrancy guards, script-instance lookup, and error handling. This is a stronger event scenario than searching generic `event` strings, which also finds FMOD audio event paths.

## Avoiding False Matches

The same string may appear in registration, logging, and invocation code. Require a function-pointer/table relationship or an actual call edge before naming an event dispatcher. A callback-looking function is not confirmed until its registration or invocation path is found.

## Metadata and Output

Rename confirmed dispatchers and callback slots, apply callback signatures incrementally, and comment the event lifecycle. Never modify executable bytes without explicit confirmation.

```text
Event/callback:
Registration:
Storage/table:
Scheduler/update loop:
Dispatcher:
Invocation target:
Arguments/lifetime:
Role and evidence:
Confidence:
```
