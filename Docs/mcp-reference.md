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
| `blueprint_validate` | Start an idempotent in-memory preflight job (`requestId` required) | read-only |
| `blueprint_apply` | Start an automatic transactional write with a unique `requestId` | destructive |
| `blueprint_job` | Query, wait for, or cancel by `jobId`/`requestId` | non-read-only |
| `blueprint_verify` | Start an idempotent compile/reload/assert job (`requestId` required) | read-only |

## Layered asset inspection

`mode` is `auto` by default. Editor mode uses Unreal object paths such as `/Game/Effects/NS_Test.NS_Test`; offline mode uses absolute `.uasset` or `.umap` file paths. To enable automatic fallback, provide both forms in the same request. An ambiguous Editor selection is never silently bypassed.

`unreal_asset_inspect` Editor facets are `support`, `generic`, `properties`, `dependencies`, `referencers`, and `specialized`. `propertyPaths` selects exact reflected values. Specialized snapshots cover Blueprint/UMG/AnimBlueprint, AnimMontage sections/slots/notifies, Material parameters/expressions, and Niagara exposed parameters/emitters.

Offline inspection uses the parser bundled under `offline/`; it reconstructs Blueprint inheritance/component trees, UMG WidgetTree evidence, Niagara parameters and serialized properties where UAssetAPI can deserialize them. Its result includes `evidence: "serialized-package"` and `editable: false`. Runtime values, Construction Script changes, and cooked or unversioned serialization still require corroboration.

`unreal_asset_inspect` and `unreal_asset_compare` accept `offlineStaging: { enabled: true, maxCachedAssets?: 64 }`. When enabled, the MCP server copies the requested `.uasset`/`.umap` packages and existing `.uexp`, `.ubulk`, and `.uptnl` companions into an isolated snapshot, verifies that the sources stayed stable during copying, and parses the copies. Successful snapshots remain in a rolling temporary cache. `maxCachedAssets` limits retained primary packages only (default 64, hard maximum 512); companion files do not consume slots. When full, the cache evicts the oldest completed snapshots and continues with the new copy instead of rejecting it because of historical usage. A compare or batch request larger than the retention limit is still copied and parsed in full, then trimmed to the configured retained count after parsing. Stable-copy and cache I/O failures return `OFFLINE_STAGING_FAILED` instead of silently reading the live file or requiring an Editor restart. Results report `used`, `sourceAssetCount`, `copiedFileCount`, `companionFileCount`, `maxCachedAssets`, `cachedAssetCount`, `evictedAssetCount`, `retention`, and `scope` under `staging`. The snapshot contains requested packages and companions, not the complete dependency graph.

`unreal_asset_compare` accepts either `baseAssetPath`/`targetAssetPath` or `baseFilePath`/`targetFilePath`. `unreal_asset_referencers` uses Asset Registry package references online; offline it requires `targetFilePath` and `searchRoot` and reports binary string matches with their encoding.

`blueprint_job wait` accepts `timeoutMs` from 0 through 600000; the MCP host timeout is 620 seconds. Successful results appear in both text and `structuredContent.result`. Failures use `structuredContent.error` with stable codes and available asset, operation, callsite, compiler, and partial-failure details.

Protocol `2.0.0` returns a `JobSnapshot` from `blueprint_validate`, `blueprint_apply`, and `blueprint_verify`. Snapshots include `method` and `durability` (`memory` for read jobs, `journal` for writes). Submitting the same `requestId`, method, and canonical parameters returns the original job; a different request under the same id fails with `REQUEST_CONFLICT`.

`blueprint_inspect` returns a facet/filter/page-independent `structureHash` with `structureHashScope: "blueprint-structure-v1"`, plus hashes for each complete requested facet. `componentQuery` filters before pagination by names, regex, classes, or inheritance and can project fields and exact template property paths. Results report both total and matched component counts.

The Operation Registry supports `component.add.initialProperties`, atomic `component.cloneRange` (one `{index}`, at most 200 components), and text or structured JSON transforms. Verify expectations can assert package dirtiness, individual or numbered component ranges, class/inheritance/parent/transform/properties, and filtered component counts.

When exactly one Editor matches, `unreal_status` returns `connected: true` and exact `session` metadata while keeping UE status fields at the result root. Session metadata includes executable identity and heartbeat. Stale descriptors are excluded from selection and returned as diagnostics. Source-control output separates the Editor provider from detected `.git`/`.svn` working-copy identity; a disabled provider reports per-file state as `unknown`.

中文：[mcp-reference.zh-CN.md](mcp-reference.zh-CN.md)
