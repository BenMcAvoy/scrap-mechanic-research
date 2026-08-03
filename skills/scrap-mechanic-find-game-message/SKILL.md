---
name: scrap-mechanic-find-game-message
description: Resolve a player-visible Scrap Mechanic message to its localization tag, find that tag in the IDA database, and trace the native code that formats, displays, logs, or emits it. Use when the user reports an in-game error, warning, toast, hint, or UI message instead of the exact binary string.
---

# Find Scrap Mechanic game messages

Use this skill when the user gives the text they see in-game, describes an action that produces a message, or asks where a warning/error is generated. The visible English sentence is often not stored in the executable: it is a localized value selected by an internal tag such as `WARNING_TUNNELING_LINE1`.

## Safety and scope

Scrap Mechanic reverse engineering and modding are authorized for this project, including singleplayer and multiplayer analysis. Use IDA Pro MCP and local game files only. Never patch executable bytes or alter instruction/data bytes in the IDB without explicit user confirmation. Renaming functions/globals, creating types/structures, adding comments, and adding non-byte-changing metadata are encouraged when justified.

## Workflow

1. Normalize the report. Preserve the exact visible text, capitalization, punctuation, placeholders, language, game mode, and the action/state that triggered it. Treat the user’s wording as a display value, not necessarily a binary literal.
2. Resolve the installation root. Start with:
   `C:\Program Files (x86)\Steam\steamapps\common\Scrap Mechanic\`
   Verify it exists. If not, inspect Steam registry settings and `steamapps\libraryfolders.vdf`, then check each library for `steamapps\common\Scrap Mechanic`.
3. Search localization files before searching IDA. Assume English unless the user specifies another language. Search every matching `InterfaceTags.txt` under the game root, prioritizing:
   - `Data\Gui\Language\English\InterfaceTags.txt`
   - `Survival\Gui\Language\English\InterfaceTags.txt`
   - `ChallengeData\Gui\Language\English\InterfaceTags.txt`
   - mod/template language folders only when the report is clearly mod-specific.
   Search the visible sentence and distinctive fragments, allowing for escaped quotes, `{}`/`%` placeholders, line breaks, and formatting markers. Also search all language directories when English has no exact match.
4. Parse the tag/value mapping and record the tag, source file, line, and exact translated value. Do not assume the first match is authoritative when the same text occurs in multiple scopes. Compare Data, Survival, ChallengeData, and mod paths.
5. Search IDA for the resolved tag, not only the displayed sentence. Use the umbrella Scrap Mechanic IDA workflow and IDA Pro MCP calls in this order:
   - `idb_list` to select the current Scrap Mechanic database;
   - `find_regex` or `search_text` for the exact tag and distinctive fragments;
   - `xrefs_to` for each string/data hit;
   - `decompile` each candidate function and inspect nearby callers/callees;
   - `func_profile` to summarize strings, calls, and function boundaries;
   - `trace_data_flow` when the tag flows through a formatter, localization lookup, UI dispatcher, logger, or network event.
6. Classify the hit. A tag reference may be:
   - the actual emitter/formatter;
   - a localization lookup table or registration site;
   - a generic UI/message helper called by many systems;
   - a diagnostic/assertion string unrelated to the player-visible message;
   - a script/data path rather than native code.
   Follow callers until the triggering subsystem and the final display/log/network call are identified.
7. Correlate behavior. Use the user’s `xyz` action to distinguish candidates. Look for nearby state checks, mode/client-server branches, manager calls, Lua callback dispatch, event IDs, and arguments that explain when the message is selected. If several paths produce the same tag, report all plausible producers and the condition that differentiates them.
8. Apply only metadata improvements without asking: rename confirmed functions/globals, label the localization tag, add comments with source path/line and confidence, and create types where the data flow supports them. Ask before any byte modification.

## Important distinctions

- The English sentence may be absent from the executable while its key is present in a resource table or native lookup call.
- A tag hit proves a relationship, not necessarily the emitter. A shared `showMessage`/`setText` helper is usually downstream; the meaningful result is the caller that chooses the tag.
- A visible message can be assembled from multiple lines/tags. Resolve each line and preserve ordering.
- Search strings case-insensitively only as a fallback; report exact casing from the files and IDA.
- Do not confuse audio `event:/...` strings, Lua script text, assertion text, or unrelated translations with the UI message unless call/data flow confirms it.

## Report format

Return:

1. Visible message and trigger context.
2. Installation root and localization files searched.
3. Resolved tag(s), English value(s), and source line(s).
4. IDA addresses/names for the tag reference, lookup/formatter, and confirmed emitter/producer.
5. Evidence chain: localization match → IDA string/data hit → xrefs → decompiled behavior → trigger correlation.
6. Confidence (`confirmed`, `probable`, or `candidate`) and the next discriminating check if unresolved.

Read [references/localization-layout.md](references/localization-layout.md) for verified paths and search rules.
