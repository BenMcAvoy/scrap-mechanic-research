---
name: scrap-mechanic-find-global
description: Locate and validate non-manager global pointers in Scrap Mechanic binaries using IDA Pro MCP. Use for ordinary globals discovered through constructor assignments, assertion strings, xrefs, or decompilation. For manager singletons, RTTI/vftables, manager variants, vfuncs, or manager member functions, use scrap-mechanic-find-manager instead.
---

# Scrap Mechanic Find Global

## Overview

Use IDA Pro MCP to identify a non-manager Scrap Mechanic global and prove what it represents. Prefer two independent discovery paths when possible: an RTTI/vftable path and an assertion-string path. Route manager singletons to `scrap-mechanic-find-manager`, which also handles their vtables and member functions.

## Operating Rules

- Work from the active IDA database session. Call `mcp__idalib__idb_list` first; use the returned `session_id` as `database` on every later call. Open the Scrap Mechanic executable with `mcp__idalib__idb_open` if no usable session exists.
- Start unfamiliar analysis with `mcp__idalib__survey_binary`, using `detail_level: "minimal"` for this large binary.
- Search with MCP tools rather than guessing addresses. Use `mcp__idalib__search_text` for rendered names/comments/disassembly and `mcp__idalib__find_regex` for strings.
- Resolve references with `mcp__idalib__xref_query` or `mcp__idalib__xrefs_to`, then inspect candidates with `mcp__idalib__decompile` and `mcp__idalib__disasm`.
- Treat an address as confirmed only when code shows the global being read, tested, assigned, or passed as the relevant object. Do not infer identity from a nearby string alone.
- Use `mcp__idalib__rename` and comments only after the evidence supports the name. Preserve the original IDA name in the report.
- Use hexadecimal addresses exactly as returned by MCP. Do not manually convert hex and decimal; use the IDA MCP integer conversion tool when conversion is required.
- Never modify executable bytes, even only inside the IDA database, without explicit user confirmation immediately before the write. Do not use patching, assembly, integer-write, or equivalent byte-mutation tools unless confirmed. Prefer analysis metadata changes such as renames, types, comments, bookmarks, and structure definitions.

## Workflow Decision

Choose the strongest available anchor:

- Use the RTTI/vftable method when the requested C++ class name is known and a COL/vftable name is present.
- Use the assertion-string method when a likely global name, `== nullptr` check, or source-file assertion is present.
- Run both methods when practical. Agreement between them is stronger evidence than either method alone.

## Method 1: RTTI/COL to Constructor Assignment

Use this method for a known class such as `CharacterManager` or `CharacterManagerClient`.

1. Search IDA names or rendered text for the RTTI/COL/vftable anchor, for example `CharacterManager::\`vft`.
2. Query xrefs to the matching name or address. Expect multiple references; prioritize a function that writes the class vftable into its first argument and initializes adjacent fields.
3. Decompile each promising candidate. Recognize a constructor by a pattern such as:

   ```cpp
   *(_QWORD *)a1 = &CharacterManager::`vftable';
   ...
   qword_141A4D570 = a1;
   ```

4. Identify the global assignment. The global may initially be named `qword_...`; do not assume every qword in the function is the manager.
5. Inspect nearby checks and calls. A check such as `if (qword_... )` followed by an assertion and then `qword_... = a1` establishes singleton initialization.
6. Record the constructor, vftable/COL, global address, and the recovered class/global name.

For `CharacterManager`, the important evidence is the assignment to `qword_141A4D570` plus the nearby assertion naming `g_contraptionCharacterManager`.

## Method 2: Assertion String to Global

Use this method when the code contains an assertion that names the object.

1. Search strings for the exact or partial expression, such as `g_contraptionCharacterManager == nullptr`, `g_contraption`, or a distinctive source-file path.
2. Cross-reference the string. Prefer code xrefs in executable segments.
3. Decompile the referencing function and locate the assertion's global operand.
4. Follow the surrounding control flow. A common singleton pattern is:

   ```cpp
   if (qword_141A4D570)
       sub_...("g_contraptionCharacterManager == nullptr", ...);
   qword_141A4D570 = a1;
   ```

5. Confirm that the same global is assigned an object of the requested class, ideally by finding its constructor or vftable initialization.
6. Record the string address, xref/function address, global address, and exact recovered name.

If a string search returns encoded data or unrelated matches, narrow the search to literal strings, source paths, or rendered disassembly and ignore the noisy hit.

## Validation and Reporting

Validate the result with at least two of the following:

- the constructor writes the requested class vftable;
- the global is assigned the constructor's `this` pointer;
- the assertion string names the global;
- xrefs show later reads of the same global in manager-related code;
- decompiled types or surrounding field initialization match the requested class.

Report results in this form:

```text
Target: CharacterManagerClient
Global: qword_XXXXXXXX (recovered name: g_contraptionCharacterManager)
Global address: 0x...
Constructor/initializer: sub_XXXXXXXX
RTTI/vftable evidence: 0x... / name
Assertion evidence: 0x... / string
Confidence: high | medium | low
Notes: explain any ambiguity or unresolved naming.
```

Rename only when requested or when the project convention clearly calls for it. Preserve the distinction between client, server, and shared managers.
