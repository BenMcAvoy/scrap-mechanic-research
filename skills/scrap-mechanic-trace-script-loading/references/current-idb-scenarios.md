# Current IDB Script-Loading Scenarios

These addresses belong to the currently analyzed Scrap Mechanic IDB and must be rediscovered for another binary version.

## LuaManager initialization

`sub_140366BC0` references `Initializing LuaManager as client`, `Initializing LuaManager as server`, and `GameInstance.cpp`. Use it to separate shared LuaManager construction from client/server mode selection.

## Mode-specific script loading

`sub_14036B2E0` references `$SURVIVAL_DATA/Scripts/game/SurvivalGame.lua`, `$GAME_DATA/Scripts/game/MenuGame.lua`, `$GAME_DATA/Scripts/game/CreativeGame.lua`, and the `seed`/`dev` keys. Its caller is `sub_140479090`. Follow its callees and search its code range for Lua load/protected-call operations before labeling it as the final executor.

The current binary also contains `luaL_loadbufferx`, `luaL_loadstring`, `lua_pcall`, `lua_call`, and `lua_setfenv` imports/strings. These are execution anchors, not proof that every caller loads a game script.
