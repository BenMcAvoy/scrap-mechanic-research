# Steam lobby browser

This is a standalone CLI browser. It does not load Scrap Mechanic, inspect
Scrap Mechanic, or use any mod code.

It dynamically loads `steam_api64.dll`, initializes Steam, obtains the
`SteamMatchMaking009` interface, requests lobbies filtered by `dh_server=1`,
and prints matching names, owners, IDs, and versions. It refreshes every ten
seconds until interrupted.

## Build

```powershell
xmake f -p windows -a x64 -m release
xmake build steam_lobby_probe
```

## Run

Copy the executable beside the same `steam_api64.dll` used by the game. Create
`steam_appid.txt` beside it containing Scrap Mechanic's AppID:

```text
387990
```

Run it while Steam is open and logged into an account that owns Scrap Mechanic:

```powershell
.\steam_lobby_probe.exe
```

Use `--once` to perform one request and exit:

```powershell
.\steam_lobby_probe.exe --once
```

`LobbyMatchList: advertised=N dedicated=M` means Steam returned `N` lobbies,
of which `M` contained `dh_server=1`. A result of zero means no matching
lobbies were visible to that Steam account at that moment.
