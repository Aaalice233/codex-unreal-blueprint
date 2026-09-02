# Architecture baseline

[简体中文](architecture.zh-CN.md)

> Planned architecture; implementation is in progress.

```text
Pi Extension / CLI
        │ localhost TCP, length-prefixed JSON-RPC 2.0
PiUnrealBlueprint Editor Plugin
        │
        ├─ Core + Operation Registry
        ├─ Transport and session discovery
        ├─ status icon / Tooltip
        └─ Commandlet (same Core)
```

## Single sources of truth

- `PLAN.md` is authoritative for product scope and completion criteria.
- The UE Core Operation Registry will be authoritative for operation names and parameter JSON Schema.
- Pi tools, CLI, interactive Editor, and Commandlet must not duplicate edit logic.

## Session and transport

The Editor will bind a random `127.0.0.1` port and publish a current-user-only session descriptor containing project path, PID, versions, capability summary, port, and a per-start token. Pi selects by canonical `.uproject` path; ambiguous sessions require an explicit selection.

Frames, nesting, and batch size are bounded. All UObject work runs on the Game Thread. Reads may coexist; writes use one heartbeat lease. Every write has a unique `requestId` and durable result state.

## Write invariant

A write is successful only after the whole affected package set passes preflight, backup, transaction, mutation, structural readback, compile, save, reload, and verification. Failure before save rolls back memory. Partial save, crash, or uncertain connection enters journal-based batch recovery. Recovery must stop rather than overwrite post-job user changes.

## Public interface baseline

The planned high-level tools are `unreal_status`, `unreal_doctor`, `unreal_search`, `blueprint_capabilities`, `blueprint_inspect`, `blueprint_validate`, `blueprint_apply`, `blueprint_job`, `blueprint_verify`, and `blueprint_history`. Detailed operation schemas are fetched from the Registry only when needed.
