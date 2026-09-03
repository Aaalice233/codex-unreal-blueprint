# MCP 工具参考

所有工具都接受可选 `session: { editorSessionId?, uproject? }`。存在多个匹配 Editor 时必须传精确 `editorSessionId`。具体 operation 字段先通过 `blueprint_capabilities` 获取，不以本文复制动态 Schema。

| 工具 | 用途 | 属性 |
|---|---|---|
| `unreal_status` | 会话、PIE、源控、脏包和队列状态 | 只读 |
| `unreal_doctor` | 插件、协议、项目、端口、权限和构建环境诊断 | 只读 |
| `unreal_search` | 搜索资产、类、成员、属性、Action 或 operation | 只读 |
| `blueprint_capabilities` | 从 Operation Registry 读取 Schema 和示例 | 只读 |
| `blueprint_inspect` | 分页读取 facet、稳定 ID、编译状态和结构 hash | 只读 |
| `blueprint_validate` | 在内存中预检一次性 operations | 只读 |
| `blueprint_apply` | 使用唯一 `requestId` 启动自动事务写入 | 破坏性 |
| `blueprint_job` | 按 `jobId`/`requestId` 查询、等待或取消 | 非只读 |
| `blueprint_verify` | 编译、重载并断言磁盘结构 | 只读 |

`blueprint_job wait` 的 `timeoutMs` 范围为 0–600000；MCP 宿主超时为 620 秒。所有成功结果同时出现在文本与 `structuredContent.result`。失败结果位于 `structuredContent.error`，包含稳定错误码及可用的资产、operation、callsite、编译和部分失败信息。

`unreal_status` 在唯一匹配时返回 `connected: true` 和精确的 `session` 元数据，并保持 UE 状态字段位于结果顶层。会话发现只读取当前进程的描述文件；实际 RPC 连接负责认证和可用性验证，不再额外创建端口探测连接。

English: [mcp-reference.md](mcp-reference.md)
