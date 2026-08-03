---
name: scrap-mechanic-find-manager
description: Locate and reverse engineer Scrap Mechanic manager singletons and their C++ classes through IDA Pro MCP. Use for managers such as CreationManager, CreationManagerClient, CharacterManager, or other RTTI-backed managers when the task involves singleton globals, constructors, RTTI/COL/vftables, virtual-function slots, client/server variants, or member functions.
---

# Scrap Mechanic Find Manager

## Purpose

Use IDA Pro MCP to identify a Scrap Mechanic manager as a class, singleton global, constructor, vtable, and set of member functions. Treat manager discovery as a class-and-inheritance problem, not as generic global lookup. Read [references/creation-manager.md](references/creation-manager.md) when using `CreationManager` as the reference case or when a client/server distinction is present.

## Safety and Mutation Boundary

- Treat Scrap Mechanic reverse engineering and modding as authorized project work, including multiplayer analysis.
- Never modify executable bytes, even only in the IDA database, without explicit user confirmation immediately before the write. Do not use `patch`, `patch_asm`, `put_int`, or equivalent byte-writing tools without that confirmation.
- Freely use evidence-based metadata operations: rename functions, globals, locals, and vtable entries; apply types; define structures; add comments and bookmarks; and save the IDB.
- Preserve original IDA names in the report and distinguish confirmed facts from hypotheses.

## Session and Tool Setup

1. Call `mcp__idalib__idb_list` and select the active Scrap Mechanic database. Pass its `session_id` as `database` on every call.
2. If no usable session exists, open the Scrap Mechanic executable with `mcp__idalib__idb_open`; initialize Hex-Rays when decompilation is needed.
3. Use `mcp__idalib__survey_binary` for unfamiliar databases, with minimal detail for the large Scrap Mechanic binary.
4. Use read-only MCP tools for discovery: `find_regex`, `search_text`, `xref_query`, `xrefs_to`, `get_bytes`, `decompile`, `disasm`, `func_profile`, `callgraph`, `callees`, and `trace_data_flow`.
5. Use `rename`, `set_type`/`type_apply_batch`, `set_comments`, and structure/type tools only for analysis metadata after evidence is sufficient.

## Manager Discovery Workflow

### 1. Establish the class identity

Search for all available anchors, in this order:

- exact RTTI/vftable names, such as `CreationManager::\`vftable'`;
- COL/typeinfo names, such as `.?AVCreationManager@@`;
- source paths, initialization/cleanup strings, and assertion strings;
- manager names in rendered disassembly and comments.

Do not assume the first matching string identifies the desired manager. Record all candidates and their addresses.

### 2. Locate the constructor or initializer

Cross-reference the RTTI/vftable and assertion/source strings. Decompile candidate functions and identify a constructor by the combination of:

- writing the class vftable to `[this]`;
- initializing fields at stable offsets;
- calling base-class or member constructors;
- checking a global for null or prior initialization;
- assigning the constructed `this` pointer to a global;
- optionally logging initialization or cleanup.

The singleton proof is the assertion-plus-assignment pattern:

```cpp
if (qword_...)
    assert("g_contraptionCreationManager == nullptr", ...);
qword_... = a1;
```

Treat the source assertion, assignment, class vftable, and constructor behavior as one evidence chain.

### 3. Distinguish manager variants automatically

When both a base manager and a client/server variant exist, compare them rather than analyzing names in isolation:

- identify each RTTI/vftable and constructor;
- check whether the derived constructor calls the base constructor first;
- find where the derived constructor replaces the vftable;
- identify separate singleton globals and assertion strings;
- compare field offsets and initialization ranges;
- compare vtable entries to classify inherited, overridden, thunk, destructor, and variant-only functions;
- use source paths and initialization logs to confirm client/server role.

For `CreationManager` and `CreationManagerClient`, the base constructor call followed by `CreationManagerClient::\`vftable'` is strong inheritance evidence. Never merge their globals merely because their names share a prefix.

### 4. Build the manager record

Record a version-aware manager record containing:

```text
Class:
Variant/role: shared | client | server | unknown
Singleton global: address, original IDA name, recovered name
Constructor: address and original name
RTTI/typeinfo/COL: addresses and names
Vtable: address and source of identification
Base class: confirmed | suspected | none
Evidence: assertions, source paths, assignments, vftable writes
Confidence: high | medium | low
```

## Vtable and Vfunc Workflow

### Enumerate slots

1. Start from the vtable address referenced by the constructor or RTTI data.
2. Read pointer-sized entries with `get_bytes` or inspect the rendered `.rdata` listing. Do not write or redefine bytes merely to make the table easier to read.
3. Resolve each entry to a function and calculate the slot index from the vtable start. Use MCP-provided addresses and the 64-bit pointer size; do not manually convert address formats.
4. Decompile or profile nontrivial entries. Record function address, original name, slot index, size, prototype, callers/callees, and referenced strings/constants.
5. Stop at a proven table boundary such as a null/invalid pointer, a following RTTI/table object, or a change to unrelated read-only data. Mark uncertain boundaries explicitly.

### Classify entries

Classify each slot as one of:

- destructor/deleting destructor;
- inherited unchanged entry;
- overridden implementation;
- adjustor/thunk/wrapper;
- pure virtual or shared stub;
- substantive manager member function;
- unresolved.

Compare base and derived vtables by slot index. An identical function pointer usually indicates inheritance; a different pointer at the same slot indicates an override or thunk and requires decompilation before assigning semantics.

### Rename and annotate

After high-confidence classification, use a stable naming convention such as:

```text
CreationManager_vfunc_09
CreationManager_addController
CreationManagerClient_vfunc_12
```

Prefer semantic names derived from strings, callers, field accesses, and behavior. Add a comment containing the class, slot index, original IDA name, and evidence. Use a dry-run rename when the MCP tool supports it, then apply metadata changes. Do not rename based on slot position alone.

## Member-Function Workflow

Use both virtual and non-virtual paths.

### Virtual members

- Start from the vtable slot and follow the function pointer.
- Inspect the prototype and `this` usage.
- Trace strings, constants, globals, and manager fields.
- Follow calls to known helpers and compare corresponding base/client implementations.
- Identify semantic behavior only after decompilation and call/data-flow evidence agree.

### Non-virtual members

- Find calls made from the constructor, initialization code, manager global users, or known vfuncs.
- Follow xrefs to manager fields and global reads/writes.
- Use `func_profile`, `callees`, `callgraph`, and `trace_data_flow` to expand from a confirmed manager function.
- Use source-file paths, log strings, assertion strings, and stable constants as semantic anchors.
- Infer a member function when the function consistently receives the manager object as `this` and accesses manager-owned fields or calls manager-specific helpers.

Apply signatures and field types incrementally. Mark inferred offsets and roles as provisional until corroborated by another function or the variant comparison.

## Reporting

Report the result in this format:

```text
Manager: CreationManagerClient
Role: client-derived manager
Singleton: qword_XXXXXXXX -> g_contraptionCreationManagerClient
Constructor: sub_XXXXXXXX
Base constructor: sub_XXXXXXXX -> CreationManager
RTTI/vftable: 0x... / name
Vtable slots mapped: N
Member functions: confirmed and provisional names
Evidence: concise chain of xrefs/decompilation/field behavior
Rejected alternatives: other manager variants or ambiguous functions
Confidence: high | medium | low
Metadata changes: names/types/comments applied
Byte writes: none
```

Always report unresolved vtable boundaries, uncertain inheritance, ambiguous slots, and version-specific assumptions.
