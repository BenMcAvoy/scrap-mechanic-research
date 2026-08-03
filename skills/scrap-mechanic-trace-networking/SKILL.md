---
name: scrap-mechanic-trace-networking
description: Trace Scrap Mechanic client/server networking, RPC, replication, authority checks, and message handlers using IDA Pro MCP. Use when a native operation must be followed across client and server code.
---

# Scrap Mechanic Trace Networking

## Purpose

Follow a networked Scrap Mechanic operation from its entry point through client/server dispatch, serialization, transport or queue handling, authority checks, and the final receiver.

## Verified Scrap Mechanic Anchors

The current binary contains `NetworkClient.cpp`, `SteamNetworkClient.cpp`, `NetworkServer.cpp`, `SteamNetworkServer.cpp`, `NetworkSendInterface.cpp`, `wrap_Network.cpp`, `NetworkClient`/`NetworkServer` RTTI, packet identifiers such as `ID_C_SCRIPT_DATA_SERVER2CLIENT`, Lua network callback diagnostics, packet-size/decompression errors, and client/server state assertions. Prefer these anchors when the target is unclear.

For exact current-IDB candidates and observed call patterns, read [references/current-idb-scenarios.md](references/current-idb-scenarios.md).

## MCP Sequence

1. Call `idb_list`; identify the active client/server or shared database context.
2. Use `find_regex` for a source path, packet identifier, callback diagnostic, or connection-state assertion.
3. Use `xrefs_to` on the string and `decompile` the referencing dispatcher/validator.
4. Use `func_profile` for callers/callees/constants, then `trace_data_flow` forward from payload creation or backward from the handler.
5. Compare client/server candidates and annotate direction, authority, and serialization boundaries. Use metadata-only edits after evidence is sufficient.

## Workflow

1. Establish the IDA session and identify whether the target is shared, client-only, server-only, or a replicated operation.
2. Search for distinctive operation names, log/assertion strings, packet/message labels, manager names, source paths, and serialization helpers.
3. Cross-reference each anchor and decompile the surrounding functions. Separate registration/dispatch code from the actual operation.
4. Trace forward from client calls and backward from server handlers with `trace_data_flow`, `xrefs_to`, `callgraph`, and `callees`.
5. Record message IDs, operation codes, channel/queue objects, argument packing, serialization calls, and callback/handler tables.
6. Identify authority gates: client prediction, server validation, ownership checks, host checks, and client-to-server versus server-to-client direction.
7. Compare client and server variants by shared base calls, matching names/constants, and corresponding handler tables.

## Concrete Binary Scenarios

### Lua network send path

Use `sub_140A67FA0` as a verified starting point. It references `wrap_Network.cpp`, checks `pNetwork->m_scriptTypeID`, validates a Lua callback name, enforces the 255-character callback-name limit, and checks packet size before reporting a send error. Follow its callers (`sub_140A68180`, `sub_140A68320`, and `sub_140A68530`) to determine how userdata is unwrapped and how the payload reaches the network layer.

### Client-to-server event manager

Use the assertion `g_contraptionClient2ServerEvents == nullptr` and source path `Client2ServerEvents.cpp` to locate `sub_1402B1250`. Treat it as a manager/registration anchor, then follow its callers and fields to find event identifiers, callback storage, and the eventual network send. Do not assume the constructor itself is the packet handler.

## Distinguishing Transport from Logic

Do not call a generic queue, serializer, or socket wrapper the gameplay implementation. Confirm the operation by following the payload into a manager, object method, state mutation, or callback. Treat message IDs as version-specific and report how they were recovered.

## Metadata and Output

Rename confirmed dispatchers/handlers, apply signatures and packet/argument types incrementally, and annotate direction and authority. Never modify executable bytes without explicit confirmation.

```text
Operation:
Direction: client->server | server->client | shared
Entry point:
Dispatcher/registration:
Message or RPC identifier:
Serialization boundary:
Authority checks:
Receiver/side effect:
Evidence and confidence:
```
