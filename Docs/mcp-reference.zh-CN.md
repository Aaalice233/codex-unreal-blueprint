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
| `unreal_viewport_list` | 列出原生 3D 视口、所属资产、尺寸与镜头状态 | 只读 |
| `unreal_viewport_capture` | 将可见视口保存为 PNG 并直接返回截图 | 只读 |
| `unreal_viewport_control` | 打开或激活视口、移动镜头、取景与恢复镜头 | 非破坏性、非只读 |
| `blueprint_capabilities` | 从 Operation Registry 读取 Schema 和示例 | 只读 |
| `blueprint_inspect` | 分页读取 facet、稳定 ID、编译状态和结构 hash | 只读 |
| `blueprint_validate` | 启动幂等的内存预检 Job（必填 `requestId`） | 只读 |
| `blueprint_apply` | 使用唯一 `requestId` 启动自动事务写入 | 破坏性 |
| `blueprint_job` | 按 `jobId`/`requestId` 查询、等待或取消 | 非只读 |
| `blueprint_verify` | 启动幂等的编译、重载和断言 Job（必填 `requestId`） | 只读 |

## 视口截图与镜头控制

先通过 `unreal_viewport_list` 选择明确的 `viewportId`。视口关闭或 Editor 重启后 ID 失效。能确认归属时，`assetPaths` 返回对应资产路径；其他预览可结合 `widgetType`、`windowTitle`、`visible` 和尺寸辨认。支持关卡及原生 3D 资产预览，包括 Blueprint 组件预览；不包含 UMG Designer、节点图、PIE 或系统窗口。Blueprint 中的 WidgetComponent 属于可截图的 3D 场景。

`unreal_viewport_capture` 要求 `viewportId` 和绝对 `.png` 文件路径 `outputPath`。使用任务报告目录中的新文件名，禁止写入 Unreal `Content` 目录或覆盖已有文件。工具按当前镜头绘制后读取真实渲染目标像素。结果包含 `evidence: "editor-viewport-pixels"`、镜头、尺寸及 `filePath`；MCP 校验 PNG 元数据后同时返回原生图片。视口关闭、不可见或尺寸为零时明确失败。截图包含渲染画面，不包含外侧工具栏。

`unreal_viewport_control` 必须携带唯一 `requestId`。通过 `blueprint_job` 等待 Journal Job 成功后再截图；响应丢失时查询原请求，不重复移动镜头。镜头控制不会修改、编译或保存资产。

| action | 参数 | 含义 |
|---|---|---|
| `open` | `assetPath` | 打开原生资产编辑器，再从返回列表选择对应视口。 |
| `activate` | `viewportId` | 打开已关闭但可用的 Blueprint 组件预览标签页，再激活所在标签页和窗口。 |
| `set_camera` | `viewportId`、`camera` | 设置 `location`、`rotation`、`lookAt`、`orthoZoom`、`fieldOfView` 中的一项或多项；传回完整 `previousCamera` 即可恢复。 |
| `pan` | `viewportId`、`delta: {x,y,z}` | 按镜头的右、上、前方向平移，使用 Unreal 单位，同时移动观察中心。 |
| `orbit` | `viewportId`、`yaw`、`pitch` | 在透视视口中绕观察中心旋转，单位为度。 |
| `zoom` | `viewportId`、正数 `factor` | 小于 1 拉近，大于 1 拉远；正交视口调整缩放范围。 |
| `frame` | `viewportId`、`boundsMin`、`boundsMax` | 立即对齐指定的世界坐标包围盒。 |

向量采用 `{x,y,z}`，旋转采用 `{pitch,yaw,roll}`。透视旋转或缩放前，观察中心必须与镜头位置不同。FOV 必须大于 0 且小于 180 度。每次镜头结果返回当前 `camera` 与调整前的 `previousCamera`，恢复需要显式调用。截图和控制前结束 PIE；打开或激活标签页后，让 Editor 完成布局，再通过 list 确认渲染尺寸。

## 分层资产检查

`mode` 默认为 `auto`。Editor 模式使用 `/Game/Effects/NS_Test.NS_Test` 这样的 Unreal 对象路径；离线模式使用绝对 `.uasset` 或 `.umap` 文件路径。要允许自动回退，需要在同一请求中同时提供两种路径。Editor 会话歧义不会被静默绕过。

`unreal_asset_inspect` 的 Editor facets 包括 `support`、`generic`、`properties`、`dependencies`、`referencers`、`specialized`。`propertyPaths` 精确选择反射属性值。专用快照覆盖 Blueprint/UMG/AnimBlueprint、AnimMontage 的 Section/Slot/Notify、Material 参数和 Expression，以及 Niagara 暴露参数和 Emitter。

离线检查使用本插件 `offline/` 内置解析器，可在 UAssetAPI 能反序列化时重建 Blueprint 继承/组件树、UMG WidgetTree 证据、Niagara 参数和序列化属性。结果固定包含 `evidence: "serialized-package"` 与 `editable: false`。运行时值、Construction Script 改动以及 cooked/unversioned 序列化仍需其他证据佐证。

`unreal_asset_inspect` 和 `unreal_asset_compare` 可传 `offlineStaging: { enabled: true, maxCachedAssets?: 64 }`。启用后，MCP 会把请求中的 `.uasset`/`.umap` 及已有的 `.uexp`、`.ubulk`、`.uptnl` companion 文件复制到隔离快照，确认复制期间源文件未变化，再解析副本。成功快照保留在滚动临时缓存中。`maxCachedAssets` 只限制缓存中保留的主 `.uasset`/`.umap` 包数量（默认 64，硬上限 512），companion 文件不占资产名额；缓存满时会自动淘汰最旧的已完成快照，然后继续复制新请求，不会因历史缓存占满而拒绝新资产。即使单次比较或批量请求中的资产数超过保留上限，也会先完整复制并解析，解析结束后才裁剪到设定数量。复制不稳定或缓存 I/O 失败会以 `OFFLINE_STAGING_FAILED` 明确失败，不会静默改读原文件或要求重启 Editor。结果的 `staging` 会报告 `used`、`sourceAssetCount`、`copiedFileCount`、`companionFileCount`、`maxCachedAssets`、`cachedAssetCount`、`evictedAssetCount`、`retention` 和 `scope`。该机制只快照请求包及 companion，不复制完整依赖图。

`unreal_asset_compare` 接受 `baseAssetPath`/`targetAssetPath` 或 `baseFilePath`/`targetFilePath`。`unreal_asset_referencers` 在线使用 Asset Registry 包引用；离线要求 `targetFilePath` 和 `searchRoot`，并返回带编码信息的二进制字符串命中。

`blueprint_job wait` 的 `timeoutMs` 范围为 0–600000；MCP 宿主超时为 620 秒。所有成功结果同时出现在文本与 `structuredContent.result`。失败结果位于 `structuredContent.error`，包含稳定错误码及可用的资产、operation、callsite、编译和部分失败信息。

协议 `2.0.0` 下，`blueprint_validate`、`blueprint_apply` 和 `blueprint_verify` 都立即返回 `JobSnapshot`。Snapshot 包含 `method` 和 `durability`（只读 Job 为 `memory`，写入为 `journal`）。相同 `requestId`、method 和规范化参数返回原 Job；同一 ID 对应不同请求时返回 `REQUEST_CONFLICT`。

终态 `blueprint_validate` 结果包含 Package 角色及原因、`compileOrder`、`timing` 和 `stats`。计时分别报告影响发现、Package 元数据检查、显式类型引用加载、Source Control、磁盘空间和编译顺序构建；统计包含 direct/compile/reference Package 数量、Asset Registry Referencer 数量，以及预检实际加载的直接 Package 数量。Validate 不会加载 reference-only 或尚未加载的 compile-only Package；每个 Package 会返回 `loadedByPreflight` 和 `checkDurationMs`。

终态 `blueprint_apply` 结果包含 `timing.totalMs`、`timing.overheadMs` 和按执行顺序排列的 `timing.phases`。`preflight`、`modify`、`compile`、`save`、`reload`、`verify` 每个阶段都会报告 `phase`、`durationMs` 和 `itemCount`。计时覆盖 UE Pipeline 本身，不包含客户端传输、队列等待和 Fixture 准备。

`blueprint_inspect` 返回不受 facet、过滤和分页影响的全局 `structureHash`，并带 `structureHashScope: "blueprint-structure-v1"` 及各完整请求 facet 的 Hash。`componentQuery` 先按名称、正则、Class 或继承关系过滤，再分页，并可选择字段与精确模板属性路径；结果同时报告组件总数和匹配数。

Operation Registry 支持 `component.add.initialProperties`、原子的 `component.cloneRange`（恰好一个 `{index}`，最多 200 个组件）以及文本/结构化 JSON Transform。verify expectation 可断言 Package Dirty、单组件或连续编号范围、Class/继承/父节点/Transform/属性，以及过滤后的组件数量。

`unreal_status` 在唯一匹配时返回 `connected: true` 和精确的 `session` 元数据，并保持 UE 状态字段位于结果顶层。Session 元数据包含可执行文件身份和心跳；过期描述不会参与选择，而是作为诊断返回。Source Control 输出区分 Editor Provider 与检测到的 `.git`/`.svn` 工作副本；Provider 未启用时文件状态为 `unknown`。

English: [mcp-reference.md](mcp-reference.md)
