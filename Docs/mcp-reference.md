# MCP tool reference

Every tool accepts optional `session: { editorSessionId?, uproject? }`. An exact `editorSessionId` is required when multiple Editors match. Fetch operation fields from `blueprint_capabilities`; this document intentionally does not duplicate the dynamic registry schema.

| Tool | Purpose | Annotation |
|---|---|---|
| `unreal_status` | Session, PIE, source-control, dirty-package, and queue status | read-only |
| `unreal_doctor` | Plugin, protocol, project, port, permission, and build diagnostics | read-only |
| `unreal_search` | Search assets, classes, members, properties, actions, or operations | read-only |
| `unreal_asset_inspect` | Layered inspection for any Unreal asset in `auto`, `editor`, or `offline` mode | read-only |
| `unreal_asset_compare` | Compare two assets online or offline | read-only |
| `unreal_asset_referencers` | Find Asset Registry or serialized binary referencers | read-only |
| `blueprint_capabilities` | Read schemas and examples from the Operation Registry | read-only |
| `blueprint_inspect` | Page facets, stable IDs, compile state, and structure hashes | read-only |
| `blueprint_validate` | Preflight one-shot operations in memory | read-only |
| `blueprint_apply` | Start an automatic transactional write with a unique `requestId` | destructive |
| `blueprint_job` | Query, wait for, or cancel by `jobId`/`requestId` | non-read-only |
| `blueprint_verify` | Compile, reload, and assert disk structure | read-only |

## Layered asset inspection

`mode` is `auto` by default. Editor mode uses Unreal object paths such as `/Game/Effects/NS_Test.NS_Test`; offline mode uses absolute `.uasset` or `.umap` file paths. To enable automatic fallback, provide both forms in the same request. An ambiguous Editor selection is never silently bypassed.

`unreal_asset_inspect` Editor facets are `support`, `generic`, `properties`, `dependencies`, `referencers`, and `specialized`. `propertyPaths` selects exact reflected values. Specialized snapshots cover Blueprint/UMG/AnimBlueprint, AnimMontage sections/slots/notifies, Material parameters/expressions, and Niagara exposed parameters/emitters.

Offline inspection uses the parser bundled under `offline/`; it reconstructs Blueprint inheritance/component trees, UMG WidgetTree evidence, Niagara parameters and serialized properties where UAssetAPI can deserialize them. Its result includes `evidence: "serialized-package"` and `editable: false`. Runtime values, Construction Script changes, and cooked or unversioned serialization still require corroboration.

`unreal_asset_compare` accepts either `baseAssetPath`/`targetAssetPath` or `baseFilePath`/`targetFilePath`. `unreal_asset_referencers` uses Asset Registry package references online; offline it requires `targetFilePath` and `searchRoot` and reports binary string matches with their encoding.

`blueprint_job wait` accepts `timeoutMs` from 0 through 600000; the MCP host timeout is 620 seconds. Successful results appear in both text and `structuredContent.result`. Failures use `structuredContent.error` with stable codes and available asset, operation, callsite, compiler, and partial-failure details.

When exactly one Editor matches, `unreal_status` returns `connected: true` and exact `session` metadata while keeping UE status fields at the result root. Discovery reads live-process descriptors only; the authenticated RPC connection verifies availability without opening a separate port-probe connection.

中文：[mcp-reference.zh-CN.md](mcp-reference.zh-CN.md)
