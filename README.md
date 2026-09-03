# Codex Unreal Blueprint

Local UE4.27 Win64 Blueprint automation for Codex. The `unreal-blueprint` Skill drives a bundled stdio MCP server, which connects to the `CodexUnrealBlueprint` Editor plugin over authenticated localhost JSON-RPC.

## Capabilities

- Search and inspect Blueprint, graph, component, UMG, AnimBlueprint, struct, and enum structure.
- Fetch strict operation schemas dynamically from the UE Operation Registry.
- Preflight and automatically execute transactional writes protected by a unique `requestId`.
- Query, wait for, or cancel jobs, then compile, reload, and verify disk structure independently.
- Preserve stable error codes, compiler output, and exact partial-failure asset lists.

## Install

Requires Node.js 22.19+, PowerShell 7, Visual Studio with C++ tools, UE4.27, and Codex Desktop/CLI:

```powershell
pwsh ./scripts/setup.ps1 `
  -UProject E:/Master/LuaSocial.uproject `
  -EngineRoot E:/UE_4.27 `
  -CodexExecutable C:/path/to/codex.exe
```

The script bundles the MCP server, safely syncs the UE plugin, and installs the plugin in the personal Codex Marketplace. Restart the Editor and open a new Codex task afterward.

Source checks:

```powershell
npm install
npm run check
```

See [setup](Docs/setup.md), [architecture](Docs/architecture.md), [MCP tools](Docs/mcp-reference.md), [product scope](Docs/product-scope.md), [source-control recovery](Docs/source-control-recovery.md), and the [release gate](Docs/v1-release-gate.md).

中文：[README.zh-CN.md](README.zh-CN.md)
