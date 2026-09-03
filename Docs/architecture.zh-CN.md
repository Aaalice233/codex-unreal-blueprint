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

MCP 负责固定工具 envelope、Codex annotations、会话选择和错误序列化；具体 operation Schema 与执行逻辑只存在于 UE Operation Registry。Editor 在随机 `127.0.0.1` 端口发布仅当前用户可读的会话描述和每次启动生成的 token。客户端按规范化 `.uproject` 或 `editorSessionId` 精确选择。

资产检查明确分为三层：

- `generic`：对任意可加载 Unreal 资产读取身份、反射属性、依赖和引用。
- `specialized`：为 Blueprint/UMG/AnimBlueprint、AnimMontage、Material/Material Instance 和 Niagara System 提供语义快照。
- `editable`：仅在资产类型接入严格 Operation Registry 与完整写入流水线时声明。

`unreal_asset_*` 支持 `auto`、`editor`、`offline`。`auto` 在请求包含 Unreal 对象路径时优先使用唯一匹配 Editor；只有没有匹配会话且提供了完整离线文件参数时才回退。会话歧义或显式指定但不存在的 `editorSessionId` 仍然报错。离线结果只属于序列化静态证据，永远不标记为可写。离线解析器随本插件安装，不再需要额外的资产检查 skill。

写入使用 `requestId` Journal。断线时客户端查询同一请求，不重放。Core 在一个写 Job 中执行预检、transaction、修改、编译、保存、重载验证和失败资产分类。

English: [architecture.md](architecture.md)
