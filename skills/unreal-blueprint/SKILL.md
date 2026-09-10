---
name: unreal-blueprint
description: Inspect UE4.26/4.27 assets online or offline and safely automate supported Blueprint writes. Use for arbitrary .uasset/.umap evidence, dependencies, referencers, comparisons, Blueprint graphs/components, UMG, AnimBlueprint, AnimMontage, Material, Material Instance, Niagara, structs, enums, interfaces, and Blueprint libraries.
license: MIT
metadata:
  compatibility: Requires Windows, PowerShell 7, and .NET 8 for offline inspection; Editor-backed inspection and writes require UE4.26/4.27 with CodexUnrealBlueprint enabled.
---

# Unreal assets and Blueprint automation

Use this package's asset, Blueprint, and viewport tools. Do not require or install a separate `inspect-unreal-uassets` skill.

## Reports and accidental imports

Write reports, raw JSON/CSV inspection output, screenshots, and helper scripts to the workspace's designated document directory, always outside every Unreal `Content` directory. If no workspace directory is specified, use an OS temporary directory outside the project. Never create an inspection-report folder under `Content/Lua`: UE can watch these files and open DataTable import dialogs.

The Editor plugin cancels only native `SCSVImportOptions` dialogs whose single displayed package path is beneath `/Game/Lua/_codex_asset_reports/`, `/Game/Lua/_codex_log_reports/`, `/Game/_codex_asset_reports/`, or `/Game/_codex_log_reports/`. It calls native Cancel during the Slate modal loop and logs the package path; it does not import a DataTable or modify auto-reimport settings. Ordinary imports, save prompts, and unrecognized dialogs remain under user control. This is recovery for legacy report locations, not permission to keep writing reports there.

## Install or update

When the tools are unavailable, the protocol or plugin version is stale, or the user asks to install/update this package:

1. Work from the `codex-unreal-blueprint` source checkout that contains `scripts/setup.ps1`. Require Windows, PowerShell 7, Node.js 22.19+, .NET SDK 8+, Visual Studio C++ tools, UE4.26/4.27, and a working Codex CLI.
2. Determine whether the update changes the UE plugin or only the Codex Skill/MCP/offline parser. For a full installation, check whether the target `.uproject` is open in Unreal Editor. Do not terminate the Editor or discard unsaved work automatically; ask the user to close that Editor before installation. The installer intentionally refuses to overwrite a loaded UE plugin.
3. From the repository root, run:

   ```powershell
   pwsh ./scripts/setup.ps1 `
     -UProject E:/path/to/Project.uproject `
     -EngineRoot E:/UE_4.27
   ```

   Add `-CodexExecutable C:/path/to/codex.exe` only when automatic Codex CLI discovery fails. Use `-Scope engine` only when the user explicitly wants an Engine-wide UE plugin instead of the default project installation. When the installed UE plugin is already current and the update changes only the Skill, MCP server, or bundled offline parser, add `-CodexOnly`; this runs checks and updates the managed Codex plugin while leaving UE files and the running Editor untouched.
4. Let the script run the applicable checks, synchronize managed files, and register the personal Marketplace entry. A full run builds and installs the Win64 plugin using the selected engine. UE4.26 projects require their matching `-EngineRoot` (for example `E:/UE_4.26`). Setup validates numeric `EngineAssociation` against `Build.version` and isolates build outputs by engine version; never install a 4.27 DLL into 4.26 or the reverse. Do not replace setup with manual partial copies. Preserve and report any prerequisite, unmanaged-file, build, validation, or registration failure.
5. After a full installation, restart Unreal Editor and create a new Codex task. After `-CodexOnly`, keep the Editor running and only create a new Codex task so the updated Skill and MCP tools are loaded. Re-run the applicable command for later updates; no separate asset-inspection skill is needed.

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

## Visual verification through Editor viewports

For visible 3D layout, clipping, scale, or placement changes, inspect properties and capture the actual native viewport instead of treating compile success as visual proof. Call `unreal_viewport_list` and select the exact session-local `viewportId` using its asset paths, widget type, window title, visibility, and dimensions. Native level and asset preview viewports are supported, including WidgetComponents displayed inside Blueprint previews. UMG Designer, node graphs, PIE, and OS windows are not supported by these tools.

Use `unreal_viewport_control` with a fresh `requestId` to `open` an asset editor, `activate` its viewport, `set_camera`, `pan`, `orbit`, `zoom`, or `frame` explicit bounds. Wait for the returned job with `blueprint_job`. Preserve `previousCamera` from the first camera adjustment and restore it with `set_camera` after verification unless the user requested the new view. Camera changes do not alter asset transforms or save assets; uncertain responses must be queried, never replayed.

`pan.delta` uses camera-local right/up/forward axes in Unreal units. `orbit.yaw` and `orbit.pitch` are degree increments around the camera's `lookAt` point. `zoom.factor` below 1 moves closer; above 1 moves farther. `frame.boundsMin` and `frame.boundsMax` are world-space bounds. Vectors use `{x,y,z}`; rotations use `{pitch,yaw,roll}`. Use `set_camera.camera` to set any of `location`, `rotation`, `lookAt`, `orthoZoom`, or `fieldOfView`, or pass the full saved camera to restore it.

Capture with `unreal_viewport_capture` using an absolute, new PNG `outputPath` in the workspace's report directory outside Content. The tool returns the real viewport image inline and as a local file. Inspect that image before claiming the layout is correct. Hidden, closed, zero-size, or unsupported viewports require explicit handling; do not substitute a thumbnail or simulated image. These are Editor preview pixels, not PIE or headset validation.

## Transactional Blueprint writes

1. Call `unreal_status` or `unreal_doctor`, then select the exact `.uproject` and `editorSessionId`; never choose the first Editor when multiple sessions match.
2. Call `unreal_search`, then `blueprint_capabilities` for the affected domain. The returned Operation Registry schema is authoritative; do not invent operation names or fields.
3. Call `blueprint_inspect` or Editor-backed `unreal_asset_inspect` and retain every affected asset's structure hash.
4. Call `blueprint_validate` with a unique `requestId` and the complete one-shot operation list. It returns an in-memory `JobSnapshot`; use `blueprint_job wait` or `query` until terminal. Validation does not create a persistent plan and does not modify assets.
5. For a write, create one unique `requestId` and call `blueprint_apply` exactly once. No confirmation dialog is required.
6. Use `blueprint_job` to query or wait. Cancel only when the reported phase is cancellation-safe. If the connection becomes uncertain, query the same `requestId`; never replay the write.
7. Finish with `blueprint_verify` using a new `requestId`, then wait for that read job and optionally use `unreal_asset_compare` against the offline or online baseline.

`blueprint_inspect.structureHash` is the global `blueprint-structure-v1` hash and is safe to pass as an exact-asset `expectedStructureHashes` value regardless of requested facets or pagination. Use `componentQuery` for precise component/property reads. Prefer one atomic `component.cloneRange` for numbered homogeneous components and set creation-time values with `component.add.initialProperties`.

A success claim requires the real Editor plugin result. Unknown operations or fields, ambiguous references, dirty packages, source-control rejection, protocol mismatch, missing Editor sessions, compile failures, and reload mismatches must remain explicit failures.

If a failure reports `partial` or `stateUnknown`, return the exact `modified`, `saved`, `notSaved`, and `unknown` asset lists plus the plugin's Git/SVN inspection guidance. The package does not provide history, restore, package copies, or automatic source-control revert. The user decides whether to restore listed assets manually.

Offline limitations must remain explicit: runtime code and Construction Script may override serialized defaults; cooked or unversioned packages may parse partially; Niagara compiled strings prove presence rather than execution; binary referencer matches require structured corroboration.
