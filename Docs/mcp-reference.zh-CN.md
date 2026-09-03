# MCP 工具参考

所有工具都接受可选 `session: { editorSessionId?, uproject? }`。存在多个匹配 Editor 时必须传精确 `editorSessionId`。具体 operation 字段先通过 `blueprint_capabilities` 获取，不以本文复制动态 Schema。

| 工具 | 用途 | 属性 |
|---|---|---|
| `unreal_status` | 会话、PIE、源控、脏包和队列状态 | 只读 |
| `unreal_doctor` | 插件、协议、项目、端口、权限和构建环境诊断 | 只读 |
| `unreal_search` | 搜索资产、类、成员、属性、Action 或 operation | 只读 |
| `unreal_asset_inspect` | 以 `auto`、`editor` 或 `offline` 分层检查任意 Unreal 资产 | 只读 |
| `unreal_asset_compare` | 在线或离线比较两个资产 | 只读 |
| `unreal_asset_referencers` | 查找 Asset Registry 或序列化二进制引用 | 只读 |
| `blueprint_capabilities` | 从 Operation Registry 读取 Schema 和示例 | 只读 |
| `blueprint_inspect` | 分页读取 facet、稳定 ID、编译状态和结构 hash | 只读 |
| `blueprint_validate` | 在内存中预检一次性 operations | 只读 |
| `blueprint_apply` | 使用唯一 `requestId` 启动自动事务写入 | 破坏性 |
| `blueprint_job` | 按 `jobId`/`requestId` 查询、等待或取消 | 非只读 |
| `blueprint_verify` | 编译、重载并断言磁盘结构 | 只读 |

## 分层资产检查

`mode` 默认为 `auto`。Editor 模式使用 `/Game/Effects/NS_Test.NS_Test` 这样的 Unreal 对象路径；离线模式使用绝对 `.uasset` 或 `.umap` 文件路径。要允许自动回退，需要在同一请求中同时提供两种路径。Editor 会话歧义不会被静默绕过。

`unreal_asset_inspect` 的 Editor facets 包括 `support`、`generic`、`properties`、`dependencies`、`referencers`、`specialized`。`propertyPaths` 精确选择反射属性值。专用快照覆盖 Blueprint/UMG/AnimBlueprint、AnimMontage 的 Section/Slot/Notify、Material 参数和 Expression，以及 Niagara 暴露参数和 Emitter。

离线检查使用本插件 `offline/` 内置解析器，可在 UAssetAPI 能反序列化时重建 Blueprint 继承/组件树、UMG WidgetTree 证据、Niagara 参数和序列化属性。结果固定包含 `evidence: "serialized-package"` 与 `editable: false`。运行时值、Construction Script 改动以及 cooked/unversioned 序列化仍需其他证据佐证。

`unreal_asset_inspect` 和 `unreal_asset_compare` 可传 `offlineStaging: { enabled: true, maxCachedAssets?: 64 }`。启用后，MCP 会把请求中的 `.uasset`/`.umap` 及已有的 `.uexp`、`.ubulk`、`.uptnl` companion 文件复制到隔离快照，确认复制期间源文件未变化，再解析副本。成功快照保留在滚动临时缓存中。`maxCachedAssets` 只限制缓存中保留的主 `.uasset`/`.umap` 包数量（默认 64，硬上限 512），companion 文件不占资产名额；缓存满时会自动淘汰最旧的已完成快照，然后继续复制新请求，不会因历史缓存占满而拒绝新资产。即使单次比较或批量请求中的资产数超过保留上限，也会先完整复制并解析，解析结束后才裁剪到设定数量。复制不稳定或缓存 I/O 失败会以 `OFFLINE_STAGING_FAILED` 明确失败，不会静默改读原文件或要求重启 Editor。结果的 `staging` 会报告 `used`、`sourceAssetCount`、`copiedFileCount`、`companionFileCount`、`maxCachedAssets`、`cachedAssetCount`、`evictedAssetCount`、`retention` 和 `scope`。该机制只快照请求包及 companion，不复制完整依赖图。

`unreal_asset_compare` 接受 `baseAssetPath`/`targetAssetPath` 或 `baseFilePath`/`targetFilePath`。`unreal_asset_referencers` 在线使用 Asset Registry 包引用；离线要求 `targetFilePath` 和 `searchRoot`，并返回带编码信息的二进制字符串命中。

`blueprint_job wait` 的 `timeoutMs` 范围为 0–600000；MCP 宿主超时为 620 秒。所有成功结果同时出现在文本与 `structuredContent.result`。失败结果位于 `structuredContent.error`，包含稳定错误码及可用的资产、operation、callsite、编译和部分失败信息。

`unreal_status` 在唯一匹配时返回 `connected: true` 和精确的 `session` 元数据，并保持 UE 状态字段位于结果顶层。会话发现只读取当前进程的描述文件；实际 RPC 连接负责认证和可用性验证，不再额外创建端口探测连接。

English: [mcp-reference.md](mcp-reference.md)
