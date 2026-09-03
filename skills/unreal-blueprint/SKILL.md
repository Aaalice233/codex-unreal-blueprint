---
name: unreal-blueprint
description: Inspect, validate, apply, monitor, and verify UE4.27 Blueprint changes through the CodexUnrealBlueprint Editor plugin. Use for Blueprint assets, graphs, components, variables, UMG, AnimBlueprint, structs, enums, interfaces, and Blueprint libraries.
license: MIT
metadata:
  compatibility: Requires Windows, UE4.27, Codex, and a running Editor with CodexUnrealBlueprint enabled.
---

# Unreal Blueprint

Use the nine package tools instead of editing `.uasset` files directly.

1. Call `unreal_status` or `unreal_doctor`, then select the exact `.uproject` and `editorSessionId`; never choose the first Editor when multiple sessions match.
2. Call `unreal_search`, then `blueprint_capabilities` for the affected domain. The returned Operation Registry schema is authoritative; do not invent operation names or fields.
3. Call `blueprint_inspect` for the required facets and retain every affected asset's structure hash.
4. Call `blueprint_validate` with the complete one-shot operation list. Validation does not create a persistent plan and does not modify assets.
5. For a write, create one unique `requestId` and call `blueprint_apply` exactly once. No confirmation dialog is required.
6. Use `blueprint_job` to query or wait. Cancel only when the reported phase is cancellation-safe. If the connection becomes uncertain, query the same `requestId`; never replay the write.
7. Finish with `blueprint_verify` using explicit asset paths and structural expectations.

A success claim requires the real Editor plugin result. Unknown operations or fields, ambiguous references, dirty packages, source-control rejection, protocol mismatch, missing Editor sessions, compile failures, and reload mismatches must remain explicit failures.

If a failure reports `partial` or `stateUnknown`, return the exact `modified`, `saved`, `notSaved`, and `unknown` asset lists plus the plugin's Git/SVN inspection guidance. The package does not provide history, restore, package copies, or automatic source-control revert. The user decides whether to restore listed assets manually.
