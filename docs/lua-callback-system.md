# Scrap Mechanic Lua callback system

## Scope and binary

This document describes the callback paths recovered from `ScrapMechanic.exe.i64` in the active IDA database.

- Image base: `0x140000000`
- SHA-256: `5d663ba2ec5dc8c7abefcc5c9344ae86f0a066c4069a91f54833524ac9a5b4f5`
- Main context: `LuaManager`
- Strong source anchors: `ContraptionCommon/LuaManager.cpp`, `LuaCallbacks.h/.cpp`

The engine creates a Lua environment, loads a mode script, creates a script instance, and later invokes methods by callback hash/name. A callback is optional: the dispatcher looks it up in the script instance and skips or reports a missing method when it is absent.

## Loading and per-script setup

`LuaManager_initialize` (`0x140366BC0`) selects client/server Lua-manager initialization. `GameScript_selectAndLoad` (`0x14036B2E0`) selects the script path and class name from game mode, builds configuration values such as `seed`, `dev`, and `worldFile`, and passes the result into the LuaManager script-loading path.

Confirmed mode paths:

| Mode | Lua path | Class name |
|---|---|---|
| Survival | `$SURVIVAL_DATA/Scripts/game/SurvivalGame.lua` | `SurvivalGame` |
| Challenge | `$CHALLENGE_DATA/Scripts/challenge/ChallengeGame.lua` | `ChallengeGame` |
| Menu | `$GAME_DATA/Scripts/game/MenuGame.lua` | `MenuGame` |
| Creative | `$GAME_DATA/Scripts/game/CreativeGame.lua` | `CreativeGame`, `CreativeFlatGame`, `ClassicCreativeGame`, `CreativeCustomGame`, or `CreativeTerrainGame` |

The load path creates the Lua script instance and installs the class/metatable. The binary contains the Lua execution imports `luaL_loadbufferx`, `luaL_loadstring`, `lua_setfenv`, and `lua_pcall`; the mode loader reaches the LuaManager execution boundary through `sub_14082E150` after selecting the path/class.

## Invocation model

The normal invocation shape is:

1. Native code creates or receives a script-instance record.
2. The record carries the Lua environment/reference, callback hash/name, script reference, and role flags.
3. A dispatcher selects a record list for the event phase.
4. The dispatcher resolves the Lua method, pushes the event arguments, and calls the Lua function.
5. The active-callback/sandbox state is cleared and Lua stack depth is checked.

The fixed-update dispatcher is `LuaManager_dispatchFixedUpdateCallbacks` (`0x14082F5E0`). It iterates server records and client records, checks the record's active/reentrancy byte, resolves the method through the Lua registry, and reports `server_onFixedUpdate` or `client_onFixedUpdate` reentrancy. `LuaManager_dispatchClientUpdateCallbacks` (`0x140830020`) performs the client update phase.

`LuaManager_dispatchLifecycleCallbacks` (`0x1408339E0`) handles create/destroy lifecycle callbacks and callback records selected by script type. `LuaManager_dispatchClientDataUpdate` (`0x14083A300`) handles client data updates. `LuaManager_createScriptInstance` (`0x140831600`) is the script-instance/class construction path and guards against creating classes during an active callback.

The generic Lua-call path resolves callback hashes and can be reached by engine events, script-to-script/network callback records, or native systems that carry a callback name. It reports missing callbacks, unknown script types, invalid script references, blacklisted generic methods, failed Lua calls, and attempts to call server callbacks in a client VM.

## Role and safety rules

`LuaSandbox_validateCallbackInvocation` (`0x140646300`) enforces the callback sandbox. It rejects invocation when no sandbox is active and rejects server functions from client callbacks. The related context helper also rejects client functions from server callbacks. A callback attempted during an active callback produces a reentrancy diagnostic rather than recursively entering the script.

`bindEventCallback`, `bindEventClientCallback`, and `bindEventServerCallback` are exposed under `sm.event`; `clearEventCallbacks`, `clearClientEventCallback`, and `clearServerEventCallback` remove them. The server binding contains an explicit host-only check. These are dynamic event callbacks, so their names are not limited to the fixed callback inventory below.

## Evidence and confidence

The callback names below are present as string constants and are referenced by callback-name/hash records. The dispatcher diagnostics and function-pointer/table relationships prove that they are callback names, not merely log text. Exact argument prototypes for most event callbacks remain version- and event-specific and are documented as `native-defined` until the corresponding producer path is traced.

Confidence: high for existence, role prefix, and dispatch eligibility; medium for exact argument lists where the producer-side event path has not yet been followed.

Open questions: fully map every producer of the non-update callback tables, recover exact Lua stack argument construction for each event, and enumerate dynamic `sm.event`/network callback names generated at runtime.
