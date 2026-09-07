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

MCP owns fixed tool envelopes, Codex annotations, session selection, and error serialization. Operation schemas and execution exist only in the UE Operation Registry. Protocol `2.0.0` makes `blueprint_validate` and `blueprint_verify` asynchronous read jobs. The Editor binds a random `127.0.0.1` port and publishes a current-user-only descriptor with a per-start token, executable identity, and an atomically refreshed five-second heartbeat. Clients reject exited, non-`UE4Editor.exe`, and heartbeat-stale descriptors, then select by canonical `.uproject` or exact `editorSessionId`.

The Editor module registers a Slate modal-loop callback independently of RPC and the ordinary Editor ticker. It recognizes native `SCSVImportOptions` content plus one package-path label in the documented legacy report directories, then invokes `OnCancel` so the import factory reports cancellation. It does not change directory watchers or close other dialogs. The callback is removed on module shutdown; each active modal window is inspected once. `CodexUnrealBlueprint.Editor.DocumentImportGuard` covers path boundaries and the real import factory's modal cancellation, consecutive reports, ordinary imports, and unrelated windows in an interactive Editor.

Asset inspection has three explicit layers:

Structure hashes inspect SCS components only for Actor-derived Blueprints. Widget and animation Blueprints contribute their specialized trees without requiring an Actor construction script.

`asset.duplicate` 将源资产作为只读影响包检查，仅将目标资产列入写入与保存集合，不编译或重存源资产及其引用者。`DuplicateSourceReadOnly` 在真实 Editor 中通过源文件哈希与 dirty 状态验证这一约束。

UMG writes accept both the exact object path and its package path, matching registry preflight. Transaction snapshots exclude generated classes and their default objects because the compiler owns their reconstruction. An operation failure is logged before attempting Undo. Reload verification permits reference-only packages to remain unloaded; loaded references must still be clean.

- `generic` reads identity, reflected properties, dependencies, and referencers for any loadable Unreal asset.
- `specialized` adds semantic snapshots for Blueprint/UMG/AnimBlueprint, AnimMontage, Material/Material Instance, and Niagara System assets.
- `editable` is reported only where an asset type is backed by the strict Operation Registry and the complete write pipeline.

`unreal_asset_*` supports `auto`, `editor`, and `offline` modes. `auto` uses a unique matching Editor when the request contains Unreal object paths, then falls back only when there is no matching session and complete offline file paths were supplied. Ambiguous sessions and a missing explicitly requested `editorSessionId` remain errors. Offline output is serialized-package evidence and is never marked editable. The offline parser is shipped inside this plugin; no separate inspection skill is required.

All long Blueprint work uses `requestId + method + canonical params` idempotency. Validate and verify jobs are memory-durable; write jobs use the Request Journal. Reusing an id for a different request is `REQUEST_CONFLICT`.

Preflight classifies packages as `directWrite`, `compileCheck`, or `referenceCheck`. Validate loads only direct-write targets; compile-only and reference-only packages are checked from Asset Registry and disk metadata, and an already-loaded dirty compile target still fails explicitly. Component mutations compile the recursive Blueprint inheritance chain while ordinary Blueprint, spawner, and ResourceMap references remain reference-only. Preflight exposes package-level progress, timing, role counts, referencer counts, and a bounded-scope failure before metadata processing grows beyond 512 packages.

A write job performs one transaction, loads and compiles direct targets and required Blueprint dependencies in dependency order, saves only direct targets, reloads the saved targets, restores clean compile-only dependencies from disk if verification compilation dirtied them, and finally checks direct assets, inherited children, and ordinary references. Source-control checkout, disk estimates, and file hashes are restricted to packages that can be saved.

中文：[architecture.zh-CN.md](architecture.zh-CN.md)
