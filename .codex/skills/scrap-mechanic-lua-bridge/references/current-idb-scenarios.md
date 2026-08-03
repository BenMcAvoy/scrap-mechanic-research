# Current IDB Lua-Bridge Scenarios

These addresses belong to the currently analyzed Scrap Mechanic IDB and must be rediscovered for another binary version.

## Network userdata wrapper cluster

The code range around `sub_140A66F90` through `sub_140A679D0` contains repeated bridge patterns:

- `sub_140A66F90` allocates userdata and applies metatables;
- `sub_140A67150`, `sub_140A67350`, and `sub_140A67560` use `luaL_checkudata`, `lua_newuserdata`, `lua_getfield`, and `lua_setmetatable`;
- `sub_140A679D0` performs repeated userdata checks and pushes a Boolean result;
- `sub_140A67B80` calls `luaL_register`, making it a strong registration-table candidate.

Compare the type-name operands and native calls across this cluster before assigning class names.

## Character vector conversion

`sub_1406425A0` is the native callback behind `Character:getTpBonePos` in the current analysis. It extracts the receiver, calls the Character manager, allocates 12 bytes of userdata, writes three components, and applies the `Vec3` metatable. Use it to demonstrate a native object lookup followed by a Lua value wrapper.
