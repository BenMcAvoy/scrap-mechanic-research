# Scrap Mechanic localization layout

This reference was verified against the user’s current installation:

`C:\Program Files (x86)\Steam\steamapps\common\Scrap Mechanic\`

## Primary files

- `Data\Gui\Language\English\InterfaceTags.txt`
- `Survival\Gui\Language\English\InterfaceTags.txt`
- `ChallengeData\Gui\Language\English\InterfaceTags.txt`

The base Data tree contains language directories including Brazilian, Chinese, English, French, German, Italian, Japanese, Korean, Polish, Russian, and Spanish. Search all language folders if the English lookup fails or if the user names another language.

## Search procedure

1. Search `InterfaceTags.txt` recursively beneath the installation root, but rank base English files above example-mod/template files.
2. Search the exact visible sentence first, then distinctive fragments after normalizing whitespace and formatting placeholders.
3. Inspect the surrounding key/value lines. Preserve the key exactly; it is the best candidate for the string search in IDA.
4. If the same value maps to multiple keys, retain every key and use the user’s trigger context plus IDA xrefs to disambiguate.
5. If no exact value exists, search related files in the same English language directory and look for multiline entries or a tag assembled by script/native code.

## Example mapping pattern

A player may report a sentence such as `You're playing the game wrong!`, while the executable references a key like `WARNING_TUNNELING_LINE1`. The localization file supplies the English sentence; the native code may only contain the key. Search the key in IDA, then follow xrefs and callers to find the selection and emission path.

## Scope cautions

`Data\ExampleMods` and `Data\ExampleMods\Templates` have their own language files. Include them when the user is analyzing one of those mods, but do not mistake a template translation for the base game message. `Cache\Fonts\English` is not the authoritative message table.
