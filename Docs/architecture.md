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

A write is successful only after the whole affected package set passes preflight, transaction, mutation, structural readback, compile, save, reload, and verification. Failure before save rolls back Editor memory through the transaction. Partial save, crash, or uncertain connection produces a precise Journal and `partial`/`stateUnknown` result. The product never copies packages or automatically restores files; users inspect and restore the listed assets through their existing Git/SVN working copy.

## Public interface baseline

The nine high-level tools are `unreal_status`, `unreal_doctor`, `unreal_search`, `blueprint_capabilities`, `blueprint_inspect`, `blueprint_validate`, `blueprint_apply`, `blueprint_job`, and `blueprint_verify`. Detailed operation schemas are fetched from the Registry only when needed. Request lookup is an internal `blueprint.request` RPC used after an uncertain write response; it is not a tenth public tool.
