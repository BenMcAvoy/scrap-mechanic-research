# Current IDB Networking Scenarios

These addresses belong to the currently analyzed Scrap Mechanic IDB and must be rediscovered for another binary version.

## Lua network callback send path

`sub_140A67FA0` references `wrap_Network.cpp`, checks `pNetwork->m_scriptTypeID`, validates a callback name, enforces the 255-character callback-name limit, and checks packet size. Its callers include `sub_140A68180`, `sub_140A68320`, and `sub_140A68530`, which use `luaL_checkudata` and are good candidates for tracing the network userdata into the send operation.

Use `func_profile` on the four functions, then decompile them and follow the payload/handle with `trace_data_flow`.

## Client-to-server event manager

`sub_1402B1250` references `Client2ServerEvents.cpp` and asserts `g_contraptionClient2ServerEvents == nullptr`. Use it as the singleton/registration anchor, then follow its caller `sub_140373460` and manager fields toward event IDs and network sends. The constructor is not automatically the packet handler.
