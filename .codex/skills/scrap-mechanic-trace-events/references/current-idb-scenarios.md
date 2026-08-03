# Current IDB Event Scenarios

These addresses belong to the currently analyzed Scrap Mechanic IDB and must be rediscovered for another binary version.

## Lua callback dispatch

- `sub_14082F5E0` references `server_onFixedUpdate callback` and `client_onFixedUpdate callback` reentrancy diagnostics.
- `sub_140830020` references `client_onUpdate callback` reentrancy diagnostics.
- `sub_14083B3B0` references `Lua call buffer - failed to call callback`.
- These functions are reached from shared higher-level LuaManager code and should be compared to recover common invocation/guard logic.

## Event registration anchors

The current binary contains `sm.event`, `bindEventCallback`, `bindEventClientCallback`, `bindEventServerCallback`, and `clearEventCallbacks`. Cross-reference these names and inspect the surrounding table/function-pointer relationships before calling them registration functions.

Avoid treating FMOD strings beginning with `event:/` as gameplay callbacks unless audio dispatch is explicitly the target.
