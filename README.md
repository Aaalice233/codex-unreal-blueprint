# pi-unreal-blueprint

> **Development status: pre-v1.0.0 / no supported release yet.** The source tree is still being completed against the evidence-based release gate. Do not use it on production assets or treat a command's presence as proof that its full UE4.27 behavior is release-ready.

[简体中文](README.zh-CN.md) · [Setup](Docs/setup.md) · [CLI/Tools](Docs/cli-reference.md) · [Product scope](Docs/product-scope.md) · [Architecture](Docs/architecture.md) · [Manual recovery](Docs/source-control-recovery.md) · [CI and release](Docs/ci-and-release.md) · [v1.0.0 gate](Docs/v1-release-gate.md)

## Product goal

`pi-unreal-blueprint` is intended to provide production-grade Blueprint automation for **Unreal Engine 4.27 on Win64** through Pi, a stable CLI, and an Unreal Editor plugin. All entry points will use one UE Core and one Operation Registry.

Planned v1 coverage includes standard Blueprints, components, variables, functions, macros, events, dispatchers, interfaces, libraries, Level Blueprints, UMG, AnimBlueprints, UserDefinedStructs, and UserDefinedEnums.

## Planned safety model

Blueprint writes will be automatic, without an extra confirmation dialog. Every write job must therefore follow one strict pipeline:

1. resolve the exact Editor session and acquire the write lease;
2. validate operations, types, references, dirty packages, source control, and disk state;
3. apply changes in an Unreal transaction;
4. compile, save, reload, and verify all affected assets;
5. retain a structured Job Journal and an exact package-state list on failure.

The product does not copy packages, create asset backups, or automatically restore Git/SVN files. A transaction can undo memory before save; partial saves or crashes require the user to inspect the reported package list and restore selected files through the existing Git/SVN working copy.

Unknown operations or fields, ambiguous targets, dirty target packages, and incompatible protocol versions must fail explicitly. A disconnected client must query the original `requestId`; it must not blindly replay a write.

## Planned interfaces

The v1 interface is planned to expose:

- Pi tools and `/unreal-blueprint` management UI;
- `pi-unreal-blueprint` CLI with human-readable and `--json` output;
- a localhost JSON-RPC 2.0 transport;
- an UE4.27 Editor plugin and a headless Commandlet using the same Core.

The source tree contains pre-release implementations of these interfaces. Only capabilities backed by the tagged UE4.27 test evidence are supported; see the [CLI/Tool reference](Docs/cli-reference.md) for current invocation syntax.

## v1 boundaries

- Supported target: UE4.27, Win64.
- Read/write scope: the complete Blueprint system listed in the product scope.
- Not in v1: UE5 support, MCP, public TypeScript SDK, standalone desktop GUI, UE Dock panel, Material, Niagara, Sequencer, or level Actor automation.
- No telemetry. Operational logs and Job Journals stay local and contain no asset backup.
- The UE plugin will not depend on, link, or distribute AQ.

## Development and releases

Public pull requests run only GitHub-hosted static/TypeScript checks. They never reach a self-hosted runner. UE4.27 compilation and E2E run only from trusted `main`, version tags, or an approved manual dispatch in protected environments.

No stable release exists. `v1.0.0` will be published only after every requirement in [the release gate](Docs/v1-release-gate.md) is backed by real UE4.27 tests; TODOs, placeholder handlers, simulated success, and silent fallback are release blockers.

Fresh-install, project-level/engine-level setup, doctor, and local development instructions are in [Docs/setup.md](Docs/setup.md). Failed writes use [manual Git/SVN recovery](Docs/source-control-recovery.md); no automatic restore is performed.

See [CONTRIBUTING.md](CONTRIBUTING.md) and [SECURITY.md](SECURITY.md).

## License

[MIT](LICENSE)
