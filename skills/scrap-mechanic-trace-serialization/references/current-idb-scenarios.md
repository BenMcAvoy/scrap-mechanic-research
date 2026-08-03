# Current IDB Serialization Scenarios

These addresses belong to the currently analyzed Scrap Mechanic IDB and must be rediscovered for another binary version.

## LuaObjectSerializer cluster

- `sub_140245780` references `LuaObjectSerializer::Deserialize was given data that is not serlized Lua data.` and has callers `sub_140241980` and `sub_14064E160`.
- `sub_14071D6A0` and `sub_14071D970` reference `LuaObjectSerializer.cpp`; the former calls the latter.
- `sub_14071D970` also references `JsonTemplate did not find any dynamic template under: {`.

Decompile all three and determine encode/decode direction from buffer reads, writes, and caller behavior. Compare their constants and recursive calls before naming fields or tags.

## UUID/network follow-up

Use `UuidNetworkMap.cpp`, `g_contraptionUuidNetworkMap`, and `wrap_Network.cpp` as separate anchors when the serialized value is a network object handle rather than a general Lua value.
