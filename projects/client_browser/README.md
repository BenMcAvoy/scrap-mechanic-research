# DedicatedHelpers client browser

The client browser reads public server records from `DedicatedHelpersClient.toml` and refreshes them through the directory service. It uses the advertised Steam ID with the game's existing `-connect_steam_id` launch flow.

The list is made from real MyGUI `Button` widgets. Hover, pressed, disabled, and click states therefore remain MyGUI-owned; the client does not inspect mouse coordinates or intercept `WM_LBUTTONUP`. A native MyGUI button click callback maps the clicked widget to the current directory record.

The public server list endpoint is intentionally REST-backed because the directory WebSocket is for authorized server relationship subscriptions, not public discovery. REST refresh is also the reconnect/failure path.

This client DLL loads the browser into Scrap Mechanic's main menu using the
game's existing `PanelEmpty` and `MenuButton` styles. Joining relaunches Scrap
Mechanic with `-connect_steam_id`, reusing the game's own connection state
machine instead of duplicating it.

Build:

```powershell
xmake f -p windows -a x64 -m release
xmake build -y
```
