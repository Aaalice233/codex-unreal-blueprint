# Codex Unreal Blueprint

Layered UE4.27 Win64 asset inspection and Blueprint automation for Codex. The `unreal-blueprint` Skill drives a bundled stdio MCP server, using either the authenticated Editor plugin or the bundled offline UAsset parser.

## Capabilities

- Inspect any `.uasset` or `.umap` at the generic serialized layer, online or offline.
- Inspect Blueprint, UMG, AnimBlueprint, AnimMontage, Material, Material Instance, and Niagara structures through specialized Editor facets.
- Compare assets and find referencers through precise Asset Registry evidence online or serialized binary evidence offline.
- Fetch strict operation schemas dynamically from the UE Operation Registry.
- Preflight and automatically execute transactional writes protected by a unique `requestId`.
- Query, wait for, or cancel jobs, then compile, reload, and verify disk structure independently.
- Preserve stable error codes, compiler output, and exact partial-failure asset lists.

## Install

Requires Node.js 22.19+, PowerShell 7, .NET SDK 8+, Visual Studio with C++ tools, UE4.27, and Codex Desktop/CLI:

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

See [setup](Docs/setup.md), [architecture](Docs/architecture.md), [MCP tools](Docs/mcp-reference.md), [source-control recovery](Docs/source-control-recovery.md), and the [release gate](Docs/v1-release-gate.md).

中文：[README.zh-CN.md](README.zh-CN.md)
