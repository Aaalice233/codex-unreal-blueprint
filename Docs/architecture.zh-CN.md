# 架构

```text
Codex task
  ├─ skills/unreal-blueprint/SKILL.md
  └─ dist/mcp/index.js (stdio)
          └─ src/client (TCP JSON-RPC 2.0)
                  └─ CodexUnrealBlueprint UE4.27 Editor plugin
                         ├─ Core / Operation Registry
                         ├─ Transport
                         ├─ Editor status
                         └─ Tests
```

MCP 负责固定工具 envelope、Codex annotations、会话选择和错误序列化；具体 operation Schema 与执行逻辑只存在于 UE Operation Registry。Editor 在随机 `127.0.0.1` 端口发布仅当前用户可读的会话描述和每次启动生成的 token。客户端按规范化 `.uproject` 或 `editorSessionId` 精确选择。

写入使用 `requestId` Journal。断线时客户端查询同一请求，不重放。Core 在一个写 Job 中执行预检、transaction、修改、编译、保存、重载验证和失败资产分类。

English: [architecture.md](architecture.md)
