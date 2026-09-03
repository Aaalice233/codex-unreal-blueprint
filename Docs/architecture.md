# Architecture

```text
Codex task
  ├─ skills/unreal-blueprint/SKILL.md
  └─ dist/mcp/index.js (stdio)
          ├─ online: src/client (TCP JSON-RPC 2.0)
          │       └─ CodexUnrealBlueprint UE4.27 Editor plugin
          │              ├─ layered asset inspection
          │              ├─ Core / Operation Registry
          │              ├─ Transport and Editor status
          │              └─ Tests
          └─ offline: offline/Inspect-UAsset.ps1
                  └─ bundled UAssetAPI inspector
```

MCP owns fixed tool envelopes, Codex annotations, session selection, and error serialization. Operation schemas and execution exist only in the UE Operation Registry. The Editor binds a random `127.0.0.1` port and publishes a current-user-only descriptor with a per-start token. Clients select by canonical `.uproject` or exact `editorSessionId`.

Asset inspection has three explicit layers:

- `generic` reads identity, reflected properties, dependencies, and referencers for any loadable Unreal asset.
- `specialized` adds semantic snapshots for Blueprint/UMG/AnimBlueprint, AnimMontage, Material/Material Instance, and Niagara System assets.
- `editable` is reported only where an asset type is backed by the strict Operation Registry and the complete write pipeline.

`unreal_asset_*` supports `auto`, `editor`, and `offline` modes. `auto` uses a unique matching Editor when the request contains Unreal object paths, then falls back only when there is no matching session and complete offline file paths were supplied. Ambiguous sessions and a missing explicitly requested `editorSessionId` remain errors. Offline output is serialized-package evidence and is never marked editable. The offline parser is shipped inside this plugin; no separate inspection skill is required.

Writes use a `requestId` journal. On connection loss the client queries that request instead of replaying it. Core performs preflight, transaction, mutation, compilation, save, reload verification, and failure asset classification within one write job.

中文：[architecture.zh-CN.md](architecture.zh-CN.md)
