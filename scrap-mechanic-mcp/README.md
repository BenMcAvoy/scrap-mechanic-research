# Scrap Mechanic MCP manager

Windows-only stdio MCP server for the local Scrap Mechanic installation. It is intended for authorized reverse-engineering and DLL-mod development in this project.

## Build

```powershell
cargo build --release
```

The executable is `target\release\scrap-mechanic-mcp.exe`.

## MCP configuration

Point the MCP client at the built executable using its absolute path:

```json
{
  "mcpServers": {
    "scrap-mechanic": {
      "command": "C:\\Users\\Ben\\scrap_headless_console\\scrap-mechanic-mcp\\target\\release\\scrap-mechanic-mcp.exe"
    }
  }
}
```

## Behavior

- Finds an already-running `ScrapMechanic.exe` and attaches to it without relaunching or changing graphics.
- Directly launches `Release\ScrapMechanic.exe` when requested, writing `Release\steam_appid.txt` with app ID `387990` first.
- Adds `-use_null_driver` by default for manager-launched processes; `keep_graphics=true` disables that default.
- Reports lifecycle events and records a non-zero exit code as a likely crash.
- Reads process memory without approval.
- Blocks memory writes until `authorize_memory_writes` is called with `confirmed=true` after explicit user approval. That authorization lasts for the MCP process session.
- Programs x64 hardware breakpoints in DR0–DR3 on current game threads and polls DR6 for hits. `get_debug_events` returns breakpoint-hit records.

Hardware breakpoint limits are Windows/x64 limits: four slots per thread, alignment/length restrictions, and possible failure when a process is protected or a thread exits during setup. New threads are not guaranteed to inherit a breakpoint until the backend reapplies it.

The server never patches executable bytes automatically. `write_memory` is an explicit, separately authorized operation and should be used only for live process data when intentionally requested.
