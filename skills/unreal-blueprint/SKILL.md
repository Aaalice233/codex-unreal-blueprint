---
name: unreal-blueprint
description: Inspect, validate, apply, verify, and recover UE4.27 Blueprint changes through the PiUnrealBlueprint Editor plugin. Use for Blueprint assets, graphs, components, variables, UMG, AnimBlueprint, structs, enums, interfaces, and Blueprint libraries.
license: MIT
compatibility: Requires Windows, UE4.27, and a running Editor with the PiUnrealBlueprint plugin.
---

# Unreal Blueprint

Use the package tools instead of editing `.uasset` files directly.

1. Call `unreal_status` or `unreal_doctor` and select the exact `.uproject` session.
2. Use `unreal_search`, then `blueprint_capabilities`, to obtain stable asset identifiers, action IDs, and operation schemas.
3. Inspect affected assets and retain their structure hashes.
4. Call `blueprint_validate` with the complete one-shot operation list.
5. For writes, generate a unique `requestId` and call `blueprint_apply` once. Never blindly replay an uncertain write.
6. Poll with `blueprint_job`; if the connection is lost, query the same `requestId`.
7. Finish with `blueprint_verify` against explicit structural expectations.
8. Use `blueprint_history` for audit or recovery. Recovery is a write and requires a new `requestId`.

The plugin performs preflight, whole-set backup, transaction, compile, save, reload verification, and recovery. A tool error means the operation did not report success; do not describe it as completed. Unknown operations, fields, ambiguous names, dirty packages, protocol mismatches, and unavailable Editor sessions must remain explicit failures.
