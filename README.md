# pi-unreal-blueprint

> **Development status: pre-v1.0.0 / not usable yet.** The repository contains early TypeScript and UE plugin scaffolding plus the documentation/CI baseline. Only a limited `unreal_status` skeleton exists; the Blueprint inspection, validation, write, Job, recovery, transport, setup, and release capabilities described below are still being implemented. Do not install this repository expecting working Blueprint automation.

[简体中文](README.zh-CN.md) · [Product scope](Docs/product-scope.md) · [Architecture](Docs/architecture.md) · [CI and release](Docs/ci-and-release.md) · [v1.0.0 gate](Docs/v1-release-gate.md)

## Product goal

`pi-unreal-blueprint` is intended to provide production-grade Blueprint automation for **Unreal Engine 4.27 on Win64** through Pi, a stable CLI, and an Unreal Editor plugin. All entry points will use one UE Core and one Operation Registry.

Planned v1 coverage includes standard Blueprints, components, variables, functions, macros, events, dispatchers, interfaces, libraries, Level Blueprints, UMG, AnimBlueprints, UserDefinedStructs, and UserDefinedEnums.

## Planned safety model

Blueprint writes will be automatic, without an extra confirmation dialog. Every write job must therefore pass strict preflight checks and execute as one recoverable unit:

1. resolve the exact Editor session and acquire the write lease;
2. validate operations, types, references, dirty packages, source control, and disk state;
3. back up the complete affected package set;
4. apply changes in an Unreal transaction;
5. compile, save, reload, and verify all affected assets;
6. recover the entire batch on failure and retain a structured Job Journal.

Unknown operations or fields, ambiguous targets, dirty target packages, and incompatible protocol versions must fail explicitly. A disconnected client must query the original `requestId`; it must not blindly replay a write.

## Planned interfaces

The v1 interface is planned to expose:

- Pi tools and `/unreal-blueprint` management UI;
- `pi-unreal-blueprint` CLI with human-readable and `--json` output;
- a localhost JSON-RPC 2.0 transport;
- an UE4.27 Editor plugin and a headless Commandlet using the same Core.

These interfaces are **not available yet**. Command examples will be added only after their implementations and tests exist.

## v1 boundaries

- Supported target: UE4.27, Win64.
- Read/write scope: the complete Blueprint system listed in the product scope.
- Not in v1: UE5 support, MCP, public TypeScript SDK, standalone desktop GUI, UE Dock panel, Material, Niagara, Sequencer, or level Actor automation.
- No telemetry. Operational logs and recovery data stay local.
- The UE plugin will not depend on, link, or distribute AQ.

## Development and releases

Public pull requests run only GitHub-hosted static/TypeScript checks. They never reach a self-hosted runner. UE4.27 compilation and E2E run only from trusted `main`, version tags, or an approved manual dispatch in protected environments.

No stable release exists. `v1.0.0` will be published only after every requirement in [the release gate](Docs/v1-release-gate.md) is backed by real UE4.27 tests; TODOs, placeholder handlers, simulated success, and silent fallback are release blockers.

See [CONTRIBUTING.md](CONTRIBUTING.md) and [SECURITY.md](SECURITY.md).

## License

[MIT](LICENSE)
