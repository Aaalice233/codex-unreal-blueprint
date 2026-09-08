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
| `unreal_viewport_list` | List native 3D viewports, owning assets, dimensions, and camera state | read-only |
| `unreal_viewport_capture` | Save a visible viewport as PNG and return its pixels inline | read-only |
| `unreal_viewport_control` | Open/activate a viewport and move, frame, or restore its camera | non-destructive, non-read-only |
| `blueprint_capabilities` | Read schemas and examples from the Operation Registry | read-only |
| `blueprint_inspect` | Page facets, stable IDs, compile state, and structure hashes | read-only |
| `blueprint_validate` | Start an idempotent in-memory preflight job (`requestId` required) | read-only |
| `blueprint_apply` | Start an automatic transactional write with a unique `requestId` | destructive |
| `blueprint_job` | Query, wait for, or cancel by `jobId`/`requestId` | non-read-only |
| `blueprint_verify` | Start an idempotent compile/reload/assert job (`requestId` required) | read-only |

## Viewport screenshots and camera control

Select an explicit `viewportId` from `unreal_viewport_list`. IDs expire when the Slate viewport closes or the Editor restarts. `assetPaths` reports verified ownership when available; `widgetType`, `windowTitle`, `visible`, and dimensions identify other native previews. Supported surfaces are level and native 3D asset viewports, including Blueprint component previews. UMG Designer, node graphs, PIE, and operating-system windows are excluded. WidgetComponents inside Blueprint previews are part of the captured 3D scene.

`unreal_viewport_capture` requires `viewportId` and an absolute `.png` `outputPath`. Use a new filename in the task's report directory, outside Unreal `Content` directories. The tool draws the current camera, reads actual render-target pixels, and writes without overwriting existing files. Results include `evidence: "editor-viewport-pixels"`, camera, dimensions, and `filePath`; MCP adds native image content after checking PNG metadata. Hidden, closed, and zero-size viewports fail explicitly. Images show the rendered scene, excluding surrounding toolbars.

`unreal_viewport_control` requires a unique `requestId`. Wait for its journaled job through `blueprint_job` before capture. Lost responses use request-journal recovery without replaying movement. Camera controls do not modify, compile, or save assets.

| action | Parameters | Meaning |
|---|---|---|
| `open` | `assetPath` | Open the native asset editor, then select its viewport from the returned list. |
| `activate` | `viewportId` | Open a closed Blueprint component preview tab when available, then activate the containing tabs and window. |
| `set_camera` | `viewportId`, `camera` | Set any of `location`, `rotation`, `lookAt`, `orthoZoom`, `fieldOfView`. Passing a previous result's full `previousCamera` restores the pose. |
| `pan` | `viewportId`, `delta: {x,y,z}` | Move along camera-local right/up/forward axes in Unreal units, moving the look-at point too. |
| `orbit` | `viewportId`, `yaw`, `pitch` | Rotate by degrees around the current look-at point; perspective only. |
| `zoom` | `viewportId`, positive `factor` | Below 1 moves closer; above 1 moves farther. Orthographic views scale their zoom extent. |
| `frame` | `viewportId`, `boundsMin`, `boundsMax` | Instantly fit an explicit world-space bounding box. |

Vectors use `{x,y,z}` and rotations use `{pitch,yaw,roll}`. Perspective orbit/zoom needs a look-at point distinct from the camera location. FOV must be strictly between 0 and 180 degrees. Each camera result returns the current `camera` and `previousCamera` for explicit restoration. Stop PIE before capture/control. Allow a newly opened/activated tab to finish layout; a subsequent list reports its render dimensions.

## Layered asset inspection

`mode` is `auto` by default. Editor mode uses Unreal object paths such as `/Game/Effects/NS_Test.NS_Test`; offline mode uses absolute `.uasset` or `.umap` file paths. To enable automatic fallback, provide both forms in the same request. An ambiguous Editor selection is never silently bypassed.

`unreal_asset_inspect` Editor facets are `support`, `generic`, `properties`, `dependencies`, `referencers`, and `specialized`. `propertyPaths` selects exact reflected values. Specialized snapshots cover Blueprint/UMG/AnimBlueprint, AnimMontage sections/slots/notifies, Material parameters/expressions, and Niagara exposed parameters/emitters.

Offline inspection uses the parser bundled under `offline/`; it reconstructs Blueprint inheritance/component trees, UMG WidgetTree evidence, Niagara parameters and serialized properties where UAssetAPI can deserialize them. Its result includes `evidence: "serialized-package"` and `editable: false`. Runtime values, Construction Script changes, and cooked or unversioned serialization still require corroboration.

`unreal_asset_inspect` and `unreal_asset_compare` accept `offlineStaging: { enabled: true, maxCachedAssets?: 64 }`. When enabled, the MCP server copies the requested `.uasset`/`.umap` packages and existing `.uexp`, `.ubulk`, and `.uptnl` companions into an isolated snapshot, verifies that the sources stayed stable during copying, and parses the copies. Successful snapshots remain in a rolling temporary cache. `maxCachedAssets` limits retained primary packages only (default 64, hard maximum 512); companion files do not consume slots. When full, the cache evicts the oldest completed snapshots and continues with the new copy instead of rejecting it because of historical usage. A compare or batch request larger than the retention limit is still copied and parsed in full, then trimmed to the configured retained count after parsing. Stable-copy and cache I/O failures return `OFFLINE_STAGING_FAILED` instead of silently reading the live file or requiring an Editor restart. Results report `used`, `sourceAssetCount`, `copiedFileCount`, `companionFileCount`, `maxCachedAssets`, `cachedAssetCount`, `evictedAssetCount`, `retention`, and `scope` under `staging`. The snapshot contains requested packages and companions, not the complete dependency graph.

`unreal_asset_compare` accepts either `baseAssetPath`/`targetAssetPath` or `baseFilePath`/`targetFilePath`. `unreal_asset_referencers` uses Asset Registry package references online; offline it requires `targetFilePath` and `searchRoot` and reports binary string matches with their encoding.

`blueprint_job wait` accepts `timeoutMs` from 0 through 600000; the MCP host timeout is 620 seconds. Successful results appear in both text and `structuredContent.result`. Failures use `structuredContent.error` with stable codes and available asset, operation, callsite, compiler, and partial-failure details.

Protocol `2.0.0` returns a `JobSnapshot` from `blueprint_validate`, `blueprint_apply`, and `blueprint_verify`. Snapshots include `method` and `durability` (`memory` for read jobs, `journal` for writes). Submitting the same `requestId`, method, and canonical parameters returns the original job; a different request under the same id fails with `REQUEST_CONFLICT`.

Terminal `blueprint_validate` results include package roles and reasons, `compileOrder`, `timing`, and `stats`. Timing separates impact discovery, package metadata checks, explicit type-reference loads, source control, disk space, and compile-order construction. Stats report direct/compile/reference package counts, Asset Registry referencer count, and how many direct packages preflight had to load. Reference-only and unloaded compile-only packages are never loaded by validate; `loadedByPreflight` and `checkDurationMs` are returned per package.

Terminal `blueprint_apply` results include `timing.totalMs`, `timing.overheadMs`, and ordered `timing.phases`. Every phase entry reports `phase`, `durationMs`, and `itemCount` for `preflight`, `modify`, `compile`, `save`, `reload`, and `verify`. These measurements cover the UE pipeline itself; client transport, queue wait, and fixture setup are outside `totalMs`.

`blueprint_inspect` returns a facet/filter/page-independent `structureHash` with `structureHashScope: "blueprint-structure-v1"`, plus hashes for each complete requested facet. `componentQuery` filters before pagination by names, regex, classes, or inheritance and can project fields and exact template property paths. Results report both total and matched component counts.

The Operation Registry supports `component.add.initialProperties`, atomic `component.cloneRange` (one `{index}`, at most 200 components), and text or structured JSON transforms. Verify expectations can assert package dirtiness, individual or numbered component ranges, class/inheritance/parent/transform/properties, and filtered component counts.

When exactly one Editor matches, `unreal_status` returns `connected: true` and exact `session` metadata while keeping UE status fields at the result root. Session metadata includes executable identity and heartbeat. Stale descriptors are excluded from selection and returned as diagnostics. Source-control output separates the Editor provider from detected `.git`/`.svn` working-copy identity; a disabled provider reports per-file state as `unknown`.

中文：[mcp-reference.zh-CN.md](mcp-reference.zh-CN.md)
