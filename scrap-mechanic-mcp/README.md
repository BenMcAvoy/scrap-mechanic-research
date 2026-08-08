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

The server includes an MCP `initialize.instructions` playbook for agent clients. It describes the autonomous build, launch, injection, event-waiting, crash-diagnosis, unload, and iteration flow. Codex clients should use `wait_for_event` for reliable blocking notification behavior.

For autonomous DLL testing, use `run_iteration` instead of manually polling. It creates a run session, records an event baseline, retries injection while the game becomes ready, observes process/debugger/artifact events concurrently, checks optional `success.file` or `success.log_pattern` markers, writes a full report, and can recover manager-launched processes. Use `get_event_cursor` plus `wait_for_event` with `after_id` when driving the lower-level tools.

- Finds an already-running `ScrapMechanic.exe` and attaches to it without relaunching or changing graphics.
- Directly launches `Release\ScrapMechanic.exe` when requested, writing `Release\steam_appid.txt` with app ID `387990` first.
- Adds `-use_null_driver` by default for manager-launched processes; `keep_graphics=true` disables that default.
- Reports lifecycle events and records a non-zero exit code as a likely crash.
- Classifies process exits, second-chance exceptions, supervisor failures, injection failures, observation timeouts, and configured success signals as distinct outcomes.
- Reads process memory without approval.
- Allows memory writes, DLL injection, and clean DLL unloading without an MCP approval gate. Operational validation and explicit cleanup-export checks remain enabled.
- Programs x64 hardware breakpoints in DR0–DR3 on current game threads and polls DR6 for hits. `get_debug_events` returns breakpoint-hit records.
- Injects and cleanly unloads x64 DLLs with the `inject` and `uninject` tools. Unloading requires an explicit cleanup export, defaulting to `ScrapMechanicMod_Unload`.
- Emits lifecycle and debugger events as `notifications/message` notifications. Clients that cannot surface server notifications can use `wait_for_event` without implementing a polling loop.
- Attaches a Windows debug supervisor to detected game processes and writes a minidump on second-chance exceptions. It also discovers `.dmp`/`.txt` artifacts emitted by the existing injected crash reporter under `%TEMP%`.
- Bounds minidump capture so a stuck dump cannot hold the debug event forever. Bugsplat is treated as an optional artifact source and is never required for supervisor operation.
- Writes lifecycle crash reports below `SCRAP_MECHANIC_MCP_REPORTS`, or `C:\Users\Ben\scrap_research\reports\scrap-mechanic` by default.

Hardware breakpoint limits are Windows/x64 limits: four slots per thread, alignment/length restrictions, and possible failure when a process is protected or a thread exits during setup. New threads are not guaranteed to inherit a breakpoint until the backend reapplies it.

The server never patches executable bytes automatically. `write_memory` is an explicit, separately authorized operation and should be used only for live process data when intentionally requested.
