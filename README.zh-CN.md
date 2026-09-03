# Codex Unreal Blueprint

面向本机 Codex 的 UE4.27 Win64 Blueprint 自动化插件。Codex 通过 `unreal-blueprint` Skill 调用本地 stdio MCP server，再通过带认证的 localhost JSON-RPC 连接 `CodexUnrealBlueprint` Editor plugin。

## 能力

- 搜索和检查 Blueprint、Graph、Component、UMG、AnimBlueprint、Struct、Enum 等资产结构。
- 从 UE Operation Registry 动态取得严格 Schema。
- 预检并自动执行带 `requestId` 幂等保护的事务写入。
- 查询、等待或取消 Job，并独立编译、重载和验证磁盘结构。
- 失败时保留稳定错误码、编译信息和精确的部分失败资产清单。

## 安装

要求 Node.js 22.19+、PowerShell 7、带 C++ 工具链的 Visual Studio、UE4.27 和 Codex Desktop/CLI：

```powershell
pwsh ./scripts/setup.ps1 `
  -UProject E:/Master/LuaSocial.uproject `
  -EngineRoot E:/UE_4.27 `
  -CodexExecutable C:/path/to/codex.exe
```

脚本构建 MCP bundle，安全同步 UE plugin，并安装到个人 Codex Marketplace。完成后重启 Editor，并新建 Codex task。

源码检查：

```powershell
npm install
npm run check
```

详细信息见权威的[产品计划](PLAN.md)、[安装](Docs/setup.zh-CN.md)、[架构](Docs/architecture.zh-CN.md)、[MCP 工具](Docs/mcp-reference.zh-CN.md)、[源控恢复](Docs/source-control-recovery.zh-CN.md)和[发布门槛](Docs/v1-release-gate.zh-CN.md)。

English: [README.md](README.md)
