# Scrap Mechanic Research Codex plugin

This repository is now a Codex plugin containing the Scrap Mechanic IDA Pro skills and the Rust game-process MCP manager.

## Local validation

```powershell
python C:\Users\Ben\.codex\skills\.system\plugin-creator\scripts\validate_plugin.py .
cargo build --release --manifest-path scrap-mechanic-mcp/Cargo.toml
```

The plugin manifest is `.codex-plugin/plugin.json`; MCP configuration is `.mcp.json`; installable skills are under `skills/`.

## Marketplace entry after pushing

Following the same split used by the IDA Pro MCP project, put an entry like this in a marketplace repository’s `.agents/plugins/marketplace.json`, replacing the URL with this repository’s real GitHub URL:

```json
{
  "name": "your-marketplace",
  "interface": { "displayName": "Your Marketplace" },
  "plugins": [
    {
      "name": "scrap-mechanic-research",
      "source": {
        "source": "url",
        "url": "https://github.com/BenMcAvoy/scrap-mechanic-research.git"
      },
      "policy": {
        "installation": "AVAILABLE",
        "authentication": "ON_INSTALL"
      },
      "category": "Developer Tools"
    }
  ]
}
```

Then install it with:

```powershell
codex plugin marketplace add <your-marketplace-repository>
codex plugin add scrap-mechanic-research@<your-marketplace-name>
```
