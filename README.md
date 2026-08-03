# Scrap Mechanic Research Codex plugin

This repository is both the installable Codex plugin and its small marketplace catalog. It contains the Scrap Mechanic IDA Pro skills and the Rust game-process MCP manager.

## Install from GitHub

```powershell
codex plugin marketplace add BenMcAvoy/scrap-mechanic-research
codex plugin add scrap-mechanic-research@benmcavoy
```

To update later:

```powershell
codex plugin marketplace upgrade benmcavoy
codex plugin remove scrap-mechanic-research@benmcavoy
codex plugin add scrap-mechanic-research@benmcavoy
```

The marketplace manifest is [.agents/plugins/marketplace.json](.agents/plugins/marketplace.json). The plugin manifest is [.codex-plugin/plugin.json](.codex-plugin/plugin.json), MCP configuration is [.mcp.json](.mcp.json), and installable skills are under [skills/](skills/).

## Local validation

```powershell
python C:\Users\Ben\.codex\skills\.system\plugin-creator\scripts\validate_plugin.py .
cargo build --release --manifest-path scrap-mechanic-mcp/Cargo.toml
```
