# CreationManager Reference Case

## Contents

- [Observed identities](#observed-identities)
- [How to distinguish the variants](#how-to-distinguish-the-variants)
- [Constructor evidence](#constructor-evidence)
- [Vtable comparison](#vtable-comparison)
- [Member-function investigation](#member-function-investigation)
- [Expected output](#expected-output)

This is a reference for the analyzed Scrap Mechanic binary. Addresses are version-specific. Re-run the discovery workflow for another executable or IDB instead of assuming they remain stable.

## Observed identities

| Role | Constructor | Singleton global | Assertion/source evidence | Vtable |
|---|---|---|---|---|
| Shared `CreationManager` | `sub_1407578C0` | `qword_141A4D568` | `g_contraptionCreationManager == nullptr`; `ContraptionCommon\\CreationManager.cpp` | `CreationManager::\`vftable'` at `0x1415EA1F8` |
| Client `CreationManagerClient` | `sub_1404113A0` | `qword_141A4D140` | `g_contraptionCreationManagerClient == nullptr`; `ScrapMechanic\\CreationManagerClient.cpp` | `CreationManagerClient::\`vftable'` at `0x1414BE9A0` |

The class typeinfo names also appear in read-only data as `.?AVCreationManager@@` and `.?AVCreationManagerClient@@`. Use them as supporting RTTI anchors, not as the only proof of a live class object.

## How to distinguish the variants

The client is not merely a second name for the shared manager:

1. `sub_1404113A0` calls `sub_1407578C0` before installing `CreationManagerClient::\`vftable'`.
2. The client constructor initializes additional state at much larger offsets, including fields around `+0x1DD0` through `+0x1E00`.
3. The client has its own singleton global and assertion string.
4. The two vtables share some function pointers but diverge at client-specific slots.
5. The source paths identify different ownership: `ContraptionCommon` for the shared manager and `ScrapMechanic` for the client manager.

Use all five signals when possible. A shared constructor call alone proves likely inheritance but does not identify every relationship or slot.

## Constructor evidence

### CreationManager

The constructor at `0x1407578C0`:

- writes `&CreationManager::\`vftable'` to `[this]`;
- initializes manager-owned fields from low offsets through approximately `+0x2C4`;
- constructs several internal containers and helper objects;
- initializes a default value of `2400` at offset `+0x2C4`;
- checks `qword_141A4D568`;
- asserts with `g_contraptionCreationManager == nullptr` and the `CreationManager.cpp` source path;
- assigns `qword_141A4D568 = this`.

This is the canonical singleton proof. Rename the global only after preserving the original IDA name and confirming the assignment is part of initialization.

### CreationManagerClient

The constructor at `0x1404113A0`:

- calls `sub_1407578C0` first;
- writes `&CreationManagerClient::\`vftable'` to `[this]`;
- initializes client-specific fields around offsets `+0x2C8` and `+0x12D8` through `+0x1300`;
- checks `qword_141A4D140`;
- asserts with `g_contraptionCreationManagerClient == nullptr` and the `CreationManagerClient.cpp` source path;
- assigns `qword_141A4D140 = this`.

Rename the client constructor only after confirming the vftable and assertion together. Do not mistake the base-constructor call for the final constructor identity.

## Vtable comparison

The observed vtable anchors are:

```text
CreationManager:       0x1415EA1F8
CreationManagerClient: 0x1414BE9A0
```

Representative pointer observations from this IDB:

| Slot | `CreationManager` | `CreationManagerClient` | Interpretation |
|---:|---|---|---|
| 0 | `0x140757AB0` | `0x140411470` | Variant-specific destructor/entry point candidate |
| 1 | `0x140757EC0` | `0x140757EC0` | Shared entry; likely inherited destructor/helper |
| 2 | `0x14022B520` | `0x140411530` | Divergent implementation; inspect both |
| 3 | `0x1407582F0` | `0x140411630` | Divergent implementation; inspect both |
| 4 | `0x14022B520` | `0x140411550` | Shared base stub versus client override candidate |
| 6 | `0x14075A760` | `0x14075A760` | Shared entry |
| 7 | `0x14075A8E0` | `0x14075A8E0` | Shared entry |
| 8 | `0x14075AAA0` | `0x14041FA0` | Divergent implementation |
| 9 | `0x14022B520` | `0x140413570` | Shared stub versus client implementation |

These are starting observations, not semantic names. For every slot:

1. derive the slot index from the vtable base and pointer size;
2. resolve the pointer to a function;
3. classify destructor, thunk, inherited, override, stub, or substantive implementation;
4. compare the corresponding base/client functions;
5. rename only when decompilation and call/data-flow evidence support the role.

Repeated `0x14022B520` entries should not automatically be named as meaningful manager methods; they may be shared stubs or unimplemented slots.

## Member-function investigation

Start from a confirmed vtable slot or from a function that accesses `qword_141A4D568`/`qword_141A4D140`. Then:

- decompile the function and identify `this`-relative field accesses;
- inspect strings, constants, assertions, and source paths;
- profile callers and callees with `func_profile`;
- use `trace_data_flow` for manager globals or distinctive fields;
- compare the client and base implementations at the same slot;
- infer field types only when multiple functions agree on offset and use;
- apply a semantic name and comment with the slot, class, and evidence.

For non-virtual members, constructor calls and manager-global xrefs are the preferred starting points. For virtual members, the vtable slot is the preferred anchor, but the slot alone is never enough to infer semantics.

Suggested names should encode variant and role when necessary:

```text
CreationManager_addController
CreationManagerClient_updateClientState
CreationManager_vfunc_09
CreationManagerClient_vfunc_12
```

Use provisional `vfunc_NN` names when the implementation is understood to belong to the class but its semantic role is not yet proven.

## Expected output

A successful analysis should identify both variants when present and include:

- each manager's singleton global and recovered semantic name;
- constructor and base-constructor relationship;
- RTTI/vtable addresses;
- class-size or field-offset observations with confidence;
- vtable slots mapped and classified;
- member functions renamed or left provisional;
- evidence and rejected alternatives;
- explicit statement that no executable bytes were modified.
