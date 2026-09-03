---
name: unreal-blueprint
description: Inspect UE4.27 assets online or offline and safely automate supported Blueprint writes. Use for arbitrary .uasset/.umap evidence, dependencies, referencers, comparisons, Blueprint graphs/components, UMG, AnimBlueprint, AnimMontage, Material, Material Instance, Niagara, structs, enums, interfaces, and Blueprint libraries.
license: MIT
metadata:
  compatibility: Requires Windows, PowerShell 7, and .NET 8 for offline inspection; Editor-backed inspection and writes require UE4.27 with CodexUnrealBlueprint enabled.
---

# Unreal assets and Blueprint automation

Use this package's twelve tools. Do not require or install a separate `inspect-unreal-uassets` skill.

## Install or update

When the tools are unavailable, the protocol or plugin version is stale, or the user asks to install/update this package:

1. Work from the `codex-unreal-blueprint` source checkout that contains `scripts/setup.ps1`. Require Windows, PowerShell 7, Node.js 22.19+, .NET SDK 8+, Visual Studio C++ tools, UE4.27, and a working Codex CLI.
2. Determine whether the update changes the UE plugin or only the Codex Skill/MCP/offline parser. For a full installation, check whether the target `.uproject` is open in Unreal Editor. Do not terminate the Editor or discard unsaved work automatically; ask the user to close that Editor before installation. The installer intentionally refuses to overwrite a loaded UE plugin.
3. From the repository root, run:

   ```powershell
   pwsh ./scripts/setup.ps1 `
     -UProject E:/path/to/Project.uproject `
     -EngineRoot E:/UE_4.27
   ```

   Add `-CodexExecutable C:/path/to/codex.exe` only when automatic Codex CLI discovery fails. Use `-Scope engine` only when the user explicitly wants an Engine-wide UE plugin instead of the default project installation. When the installed UE plugin is already current and the update changes only the Skill, MCP server, or bundled offline parser, add `-CodexOnly`; this runs checks and updates the managed Codex plugin while leaving UE files and the running Editor untouched.
4. Let the script run the applicable checks, synchronize managed files, and register the personal Marketplace entry. A full run also builds and installs the UE4.27 Win64 plugin. Do not replace this with manual partial copies. Preserve and report any prerequisite, unmanaged-file, build, validation, or registration failure.
5. After a full installation, restart Unreal Editor and create a new Codex task. After `-CodexOnly`, keep the Editor running and only create a new Codex task so the updated Skill and all twelve MCP tools are loaded. Re-run the applicable command for later updates; no separate asset-inspection skill is needed.

Verify both paths after installation:

- With the Editor closed, call `unreal_asset_inspect` with `mode: "offline"` and an absolute `filePath`; require `mode: "offline"`, `evidence: "serialized-package"`, and `facets.support.editable: false`.
- With the target Editor open, call `unreal_status`, then `unreal_asset_inspect` with `mode: "editor"` and an Unreal `assetPath`; require the selected session and `mode: "editor"`.
- To verify automatic routing, provide both `assetPath` and `filePath` with `mode: "auto"`. It must use the unique matching Editor or fall back offline only when no matching Editor exists; ambiguity must remain an error.

## Choose the inspection layer

1. Use `unreal_asset_inspect` for every asset type.
2. Prefer `mode: "auto"` when both an Unreal object path and absolute asset file path are known. It uses a unique matching Editor and falls back to the bundled offline parser only when no matching Editor exists.
3. Use `mode: "editor"` when current WidgetTree, AnimGraph, Material, Montage, Niagara, reflected values, or precise Asset Registry data matters.
4. Use `mode: "offline"` when the Editor is closed or serialized disk evidence is specifically required. Offline results are read-only and do not prove runtime behavior.
5. Read `facets.support`: `generic` applies to every loadable asset, `specialized` means a semantic inspector exists, and `editable` means the asset type is backed by the strict write pipeline.

For an asset that requires offline parsing, or when the Editor may hold its package open, use `offlineStaging: { "enabled": true }` on `unreal_asset_inspect` or `unreal_asset_compare` before asking to close or restart the Editor. The MCP server copies only the requested `.uasset`/`.umap` packages and their existing `.uexp`, `.ubulk`, and `.uptnl` companions into an isolated snapshot, verifies that each source stayed stable during copying, and parses the copies. Successful snapshots are retained in a rolling temporary cache so repeated offline work does not require touching the live package again.

`maxCachedAssets` limits the number of retained primary `.uasset`/`.umap` packages in that cache, solely to prevent the temporary folder from growing indefinitely. It defaults to 64 and has a hard maximum of 512. Companion files do not consume asset slots. When the cache is full, the server evicts the oldest completed snapshots and continues copying the new request; even a single compare/batch request larger than the retention limit is parsed in full and trimmed only after parsing. Historical cache usage or request width must never be reported as a reason that a new asset cannot be staged. Read the returned `staging` object and require `used: true` plus `retention: "rolling-cache"` before claiming isolated parsing; `cachedAssetCount` and `evictedAssetCount` describe cache maintenance. A stable-copy or cache I/O failure returns `OFFLINE_STAGING_FAILED` and must not silently fall back to the live file. Staging covers requested packages and companions, not their complete dependency graph, so dependency and runtime conclusions retain the normal offline evidence limits.

Use `unreal_asset_compare` for before/after or sibling assets. Use `unreal_asset_referencers` for references: Editor mode is authoritative Asset Registry evidence; offline mode is a bounded binary search and must be described as serialized string evidence.

Specialized Editor inspection covers Blueprint/UMG/AnimBlueprint, AnimMontage sections/slots/notifies, Material parameters/expressions, and Niagara exposed parameters/emitters. Offline inspection additionally reconstructs locally available Blueprint inheritance/component trees and extracts UMG/Niagara serialized evidence where UAssetAPI can deserialize it.

## Blueprint write workflow

1. Call `unreal_status` or `unreal_doctor`, then select the exact `.uproject` and `editorSessionId`; never choose the first Editor when multiple sessions match.
2. Call `unreal_search`, then `blueprint_capabilities` for the affected domain. The returned Operation Registry schema is authoritative; do not invent operation names or fields.
3. Call `blueprint_inspect` or Editor-backed `unreal_asset_inspect` and retain every affected asset's structure hash.
4. Call `blueprint_validate` with the complete one-shot operation list. Validation does not create a persistent plan and does not modify assets.
5. For a write, create one unique `requestId` and call `blueprint_apply` exactly once. No confirmation dialog is required.
6. Use `blueprint_job` to query or wait. Cancel only when the reported phase is cancellation-safe. If the connection becomes uncertain, query the same `requestId`; never replay the write.
7. Finish with `blueprint_verify`, then optionally use `unreal_asset_compare` against the offline or online baseline.

A success claim requires the real Editor plugin result. Unknown operations or fields, ambiguous references, dirty packages, source-control rejection, protocol mismatch, missing Editor sessions, compile failures, and reload mismatches must remain explicit failures.

If a failure reports `partial` or `stateUnknown`, return the exact `modified`, `saved`, `notSaved`, and `unknown` asset lists plus the plugin's Git/SVN inspection guidance. The package does not provide history, restore, package copies, or automatic source-control revert. The user decides whether to restore listed assets manually.

Offline limitations must remain explicit: runtime code and Construction Script may override serialized defaults; cooked or unversioned packages may parse partially; Niagara compiled strings prove presence rather than execution; binary referencer matches require structured corroboration.
