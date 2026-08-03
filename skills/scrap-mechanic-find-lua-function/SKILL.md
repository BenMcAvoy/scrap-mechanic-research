---
name: scrap-mechanic-find-lua-function
description: Locate and identify native C++ functions exposed to Scrap Mechanic Lua through IDA Pro MCP. Use for object methods such as Character:getTpBonePos and namespaced or static functions such as sm.body.createBody, especially when the Lua name appears in multiple registration tables.
---

# Scrap Mechanic Find Lua Function

## Purpose

Find the native C++ implementation behind a Scrap Mechanic Lua function by following its name string into a Lua registration table, resolving the adjacent function pointer, and validating the candidate through its Lua API behavior and registration context. Distinguish duplicate Lua names by table context rather than selecting the first string xref.

## Operating Rules

- Use the active IDA session and pass its `session_id` as `database` on every MCP call.
- Search for the Lua name as a string first. Use `mcp__idalib__find_regex` for exact string matches and `mcp__idalib__search_text` to locate rendered `.rdata` registration entries.
- Resolve all xrefs to each string. A duplicate name is expected; the string address is not the registration entry and does not identify the native implementation by itself.
- Read the surrounding `.rdata` bytes or rendered listing to recover adjacent name/function pointer pairs. Do not modify bytes.
- Decompile candidate functions before naming them. Use Lua stack behavior, argument checks, userdata/metatable handling, global manager accesses, and nearby registration context as evidence.
- Metadata edits are encouraged: rename the native function, rename the Lua state argument to `L`, rename meaningful locals/globals, apply types, and add comments when the evidence is sufficient. Never patch executable bytes without explicit confirmation under the umbrella skill's byte-modification rule.

## Core Registration-Table Pattern

A native Lua registration entry commonly appears as adjacent pointers in `.rdata`:

```asm
.rdata:...                 dq offset aGettpbonepos ; "getTpBonePos"
.rdata:...                 dq offset sub_1406425A0
```

Treat the first pointer as the Lua-visible name and the next pointer as the native callback candidate. Confirm the pairing by reading enough surrounding entries to identify the table's repeated record layout. The exact record may contain additional fields, but a name pointer immediately followed by a code pointer is a strong initial anchor.

## Workflow

1. Normalize the requested Lua path and leaf name. For `Character:getTpBonePos`, the leaf is `getTpBonePos` and the receiver is `Character`. For `sm.body.createBody`, the leaf is `createBody` and the namespace path is `sm.body`.
2. Search the string table for the exact leaf name. Record every string address and every xref location.
3. For each xref, inspect the `.rdata` location and nearby entries. Recover the adjacent callback pointer using the rendered listing, `get_bytes`, or an equivalent read-only MCP query.
4. Resolve the callback pointer to a function and decompile it. Reject candidates that are not functions, are generic wrappers without the requested registration context, or do not behave like a Lua C callback.
5. Use the registration table and surrounding entries to distinguish duplicates. Continue searching until the table context matches the requested receiver or namespace.
6. Validate the candidate using the function's Lua-facing behavior, then rename and annotate it if the identity is sufficiently established.
7. Report all duplicate candidates considered and explain why the selected one belongs to the requested class or namespace.

## Distinguishing Duplicate Method Names

For `Character:getTpBonePos`, multiple `getTpBonePos` strings may exist because another type, such as `Tool`, exposes a method with the same leaf name. Distinguish them using these signals, in descending order of strength:

1. **Registration-table context:** identify the table or initializer that owns the entry. Look for neighboring method names, type names, namespace strings, metatable setup, or a registration function associated with `Character` versus `Tool`.
2. **Receiver extraction:** inspect the first Lua argument. A helper commonly extracts a userdata/object from stack index 1. Follow its type checks, metatable checks, vftable use, or object layout to identify the receiver class.
3. **Native object usage:** inspect calls made through the extracted receiver. A Character method should use character/manager state; a Tool method should use tool-related state. Treat this as supporting evidence, not proof by name alone.
4. **Argument and return contract:** compare `lua_gettop`, `luaL_check*`, userdata extraction, and pushed return values. Matching the known Lua API signature helps select the correct overload.
5. **Neighboring registrations:** use adjacent entries as a fingerprint. A table containing character methods is stronger evidence than an isolated duplicate name.

Do not choose based only on the callback address being nearby in memory. Registration-table proximity is useful only after confirming the table's boundaries and record pattern.

## Object-Method Recognition

For an object method such as `Character:getTpBonePos`, expect a native callback shaped approximately like:

```cpp
__int64 __fastcall sub_XXXXXXXX(__int64 a1)
{
    // a1 is the Lua state; rename it to L
    // stack index 1 contains the Character receiver
    // later indices contain method arguments
}
```

In the example `getTpBonePos` implementation, useful evidence includes:

- an exact Lua stack-count check for two arguments;
- extraction of the receiver from stack index 1;
- a string argument checked at stack index 2;
- access through `g_contraptionCharacterManager`;
- construction of a three-component userdata value;
- assignment of the `Vec3` metatable;
- `lua_pushnil` on lookup failure.

These details support the interpretation that the callback is a Character-facing Lua method returning a vector, but the registration table still determines whether it is the Character or Tool duplicate.

## Namespaced or Static Functions

For `sm.body.createBody`, search for `createBody` and apply the same name-pointer/function-pointer workflow. Because static or namespaced functions have no receiver userdata, distinguish duplicates using:

- the namespace registration initializer or table owner;
- nearby namespace/table strings such as `sm`, `body`, or related body API names;
- the function's argument and return behavior;
- calls into body/physics subsystems;
- neighboring entries that form the `sm.body` API group.

Do not require a receiver check for namespaced functions. Instead, prove the registration table represents the requested namespace and that the implementation's behavior is consistent with it.

## Renaming and Annotation

When the identity is confirmed, use `mcp__idalib__rename` with a dry run first, then rename the function to a stable project convention, for example:

```text
lua_Character_getTpBonePos
lua_sm_body_createBody
```

Rename the Lua state parameter from `a1` to `L` using the local-variable rename operation. Rename other locals only when their roles are clear, such as `character`, `boneName`, `result`, or `vec3`. Add a comment containing the Lua path, registration-entry address, and evidence. Preserve the original IDA function name in the report.

## Validation and Reporting

Require at least two independent confirmations:

- name pointer and adjacent callback pointer form a valid registration entry;
- registration-table context matches the requested receiver or namespace;
- decompiled callback has matching Lua argument/return behavior;
- receiver extraction or subsystem usage matches the requested class;
- neighboring entries corroborate the table identity.

Report:

```text
Lua target: Character:getTpBonePos
Native function: lua_Character_getTpBonePos
Address: 0x...
Original IDA name: sub_XXXXXXXX
Registration entry: 0x...
Name string: 0x...
Registration context: Character / Tool / sm.body / unresolved
Evidence: concise list
Rejected duplicates: addresses and reasons
Confidence: high | medium | low
```

If the table owner cannot be proven, report the candidate as unresolved rather than silently assigning it to the requested class.
