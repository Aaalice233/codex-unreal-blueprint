# 架构

```text
Codex task
  ├─ skills/unreal-blueprint/SKILL.md
  └─ dist/mcp/index.js (stdio)
          ├─ 在线：src/client (TCP JSON-RPC 2.0)
          │       └─ CodexUnrealBlueprint UE4.27 Editor plugin
          │              ├─ 分层资产检查
          │              ├─ Core / Operation Registry
          │              ├─ Transport 与 Editor 状态
          │              └─ Tests
          └─ 离线：offline/Inspect-UAsset.ps1
                  └─ 随包 UAssetAPI 解析器
```

MCP 负责固定工具 envelope、Codex annotations、会话选择和错误序列化；具体 operation Schema 与执行逻辑只存在于 UE Operation Registry。协议 `2.0.0` 将 `blueprint_validate` 和 `blueprint_verify` 定义为异步只读 Job。Editor 在随机 `127.0.0.1` 端口发布仅当前用户可读的会话描述、每次启动生成的 token、可执行文件身份和每 5 秒原子刷新的心跳。客户端排除进程退出、非 `UE4Editor.exe` 或心跳过期的描述，再按规范化 `.uproject` 或 `editorSessionId` 精确选择。

资产检查明确分为三层：

- `generic`：对任意可加载 Unreal 资产读取身份、反射属性、依赖和引用。
- `specialized`：为 Blueprint/UMG/AnimBlueprint、AnimMontage、Material/Material Instance 和 Niagara System 提供语义快照。
- `editable`：仅在资产类型接入严格 Operation Registry 与完整写入流水线时声明。

`unreal_asset_*` 支持 `auto`、`editor`、`offline`。`auto` 在请求包含 Unreal 对象路径时优先使用唯一匹配 Editor；只有没有匹配会话且提供了完整离线文件参数时才回退。会话歧义或显式指定但不存在的 `editorSessionId` 仍然报错。离线结果只属于序列化静态证据，永远不标记为可写。离线解析器随本插件安装，不再需要额外的资产检查 skill。

所有长时间 Blueprint 工作都以 `requestId + method + canonical params` 保证幂等。validate/verify Job 仅在内存中保存，写 Job 使用 Request Journal；同一 ID 对应不同请求时返回 `REQUEST_CONFLICT`。

预检把 Package 分为 `directWrite`、`compileCheck` 和 `referenceCheck`。Validate 只加载直接写入目标；compile-only 与 reference-only Package 仅通过 Asset Registry 和磁盘元数据检查，但已经加载且标脏的编译目标仍会明确失败。组件修改只编译递归 Blueprint 继承链，普通 Blueprint、召唤器和 ResourceMap 引用保持为 reference-only。预检会报告逐 Package 进度、耗时、角色数量和 Asset Registry Referencer 数量，并在元数据处理范围超过 512 个 Package 前明确失败。

写 Job 只使用一个 transaction，按依赖顺序加载并编译直接目标和必要 Blueprint 依赖，只保存直接目标，重载已保存目标；若验证编译把原本干净的 compile-only 依赖标脏，则从磁盘恢复，最后验证直接资产、子 Blueprint 继承结果和普通引用。Source Control checkout、磁盘估算和文件 Hash 仅覆盖可能保存的 Package。

English: [architecture.md](architecture.md)
