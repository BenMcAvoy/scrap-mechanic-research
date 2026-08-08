# Lua callback inventory

The following names were recovered from the current IDB. A method is callable when the corresponding script instance exposes it and the native producer reaches the matching dispatcher/table. Missing methods are skipped or logged; defining a method does not itself cause an event to be produced.

## Lifecycle and update callbacks

| Callback | Role | Call path / condition | Arguments |
|---|---|---|---|
| `server_onCreate` | server | script instance creation on server | native-defined |
| `client_onCreate` | client | script instance creation on client | native-defined |
| `server_onDestroy` | server | server script teardown | native-defined |
| `client_onDestroy` | client | client script teardown | native-defined |
| `server_onRefresh` | server | script refresh/reload path | native-defined |
| `client_onRefresh` | client | script refresh/reload path | native-defined |
| `server_onFixedUpdate` | server | fixed-update record list, `0x14082F5E0` | native-defined, fixed-step context |
| `client_onFixedUpdate` | client | fixed-update record list, `0x14082F5E0` | native-defined, fixed-step context |
| `client_onUpdate` | client | client update dispatcher, `0x140830020` | native-defined, frame context |
| `server_onReceiveUpdate` | server | server receive-update list, fixed-update dispatcher cluster | native-defined |
| `client_onClientDataUpdate` | client | client-data dispatcher, `0x14083A300` | native-defined |
| `client_onLocalPlayerChangedWorld` | client | local-player world-change producer | native-defined |

## World, terrain, and cell callbacks

| Callback | Role |
|---|---|
| `server_onUnload` | server |
| `server_onTerrainCreated` | server |
| `server_onTerrainLoaded` | server |
| `client_onTerrainCreated` | client |
| `client_onTerrainLoaded` | client |
| `server_onCellCreated` | server |
| `server_onCellLoaded` | server |
| `server_onCellUnloaded` | server |
| `client_onCellLoaded` | client |
| `client_onCellUnloaded` | client |
| `server_onWorldChanged` | server |
| `client_onChildJointRemoved` | client |

These are stored as event-name/hash entries and are eligible for dispatch only when the associated world/cell/contraption producer reaches the corresponding table.

## Interaction, combat, construction, and inventory callbacks

| Callback | Role |
|---|---|
| `server_onInteractableCreated` | server |
| `server_onInteractableDestroyed` | server |
| `server_onProjectile` | server |
| `server_onExplosion` | server |
| `server_onMelee` | server |
| `server_onProjectileFire` | server |
| `server_onCollision` | server |
| `client_onCollision` | client |
| `client_onProjectile` | client |
| `server_onVoxelDestruction` | server |
| `server_onVoxelConstruction` | server |
| `server_onMining` | server |
| `client_onInteract` | client |
| `client_onCancel` | client |
| `client_onSkipDialog` | client |
| `client_onReload` | client |
| `server_onCollisionCrush` | server |
| `server_onShapeRemoved` | server |
| `server_onInventoryChanges` | server |
| `client_onTinker` | client |
| `client_onInteractThroughJoint` | client |
| `client_onAction` | client |
| `server_onSledgehammer` | server |
| `client_onMelee` | client |

The binary also contains `server_onCharacterChangedColor`, `server_onUnitUpdate`, `server_onIgnite`, `server_onRemoved`, and `server_onFloating`; these are native-defined gameplay notifications in the same callback-name inventory.

## Player, game-mode, graphics, and tool callbacks

| Callback | Role |
|---|---|
| `server_onPlayerJoined` | server |
| `server_onPlayerLeft` | server |
| `server_onReset` | server |
| `server_onRestart` | server |
| `server_onSaveLevel` | server |
| `server_onTestLevel` | server |
| `server_onStopTest` | server |
| `client_onLoadingScreenLifted` | client |
| `client_onLanguageChange` | client |
| `client_onUnstuck` | client |
| `client_onGraphicsLoaded` | client |
| `client_onEvent` | client |
| `client_onGraphicsUnloaded` | client |
| `client_onEquip` | client |
| `client_onUnequip` | client |
| `client_onToggle` | client |
| `client_onEquippedUpdate` | client |

`client_onGraphicsLoaded` is a real phase boundary: the native animation wrapper warns when `getAnimationInfo` is used outside this callback.

## Dynamic callbacks

The finite names above are not the whole callable surface. The engine also supports:

- `sm.event` callbacks bound with `bindEventCallback`, `bindEventClientCallback`, or `bindEventServerCallback`.
- Generic callback records whose name/hash is carried in a Lua call buffer or event record. The dispatcher can call any permitted method that exists in the target script instance.
- Network/script callbacks whose names are serialized with a maximum length of 255 characters. Server-only records are rejected in a client VM.
- GUI, animation, effect, and other native callback registrations, including `bindAnimationCallback`, `removeAnimationCallbacks`, GUI `set*Callback` methods, and `setGarage*Callback` methods. These are subsystem callbacks rather than the per-script gameplay lifecycle table.

## Native dispatch entities

| Address | Renamed entity | Evidence |
|---|---|---|
| `0x14036B2E0` | `GameScript_selectAndLoad` | mode script paths and LuaManager load boundary |
| `0x140366BC0` | `LuaManager_initialize` | client/server initialization diagnostics |
| `0x14082F5E0` | `LuaManager_dispatchFixedUpdateCallbacks` | fixed-update callback diagnostics and record iteration |
| `0x140830020` | `LuaManager_dispatchClientUpdateCallbacks` | `client_onUpdate` reentrancy diagnostic |
| `0x1408339E0` | `LuaManager_dispatchLifecycleCallbacks` | create/destroy callback diagnostics |
| `0x14083A300` | `LuaManager_dispatchClientDataUpdate` | `client_onClientDataUpdate` diagnostic |
| `0x140831600` | `LuaManager_createScriptInstance` | class creation and callback-state guard |
| `0x140646300` | `LuaSandbox_validateCallbackInvocation` | sandbox and client/server role checks |
| `0x14083B1A0` | `LuaManager_rejectServerCallbackInClient` | explicit client-VM server-callback diagnostic |
| `0x14083B3B0` | `LuaManager_reportCallbackCallFailure` | failed callback diagnostic |
| `0x14083B800` | `LuaManager_reportMissingCallback` | callback-not-found diagnostic |

The IDB was saved after these metadata renames and function comments were added.
