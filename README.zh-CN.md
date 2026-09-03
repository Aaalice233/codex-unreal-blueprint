# Codex Unreal Blueprint

面向本机 Codex 的 UE4.27 Win64 分层资产检查与 Blueprint 自动化插件。`unreal-blueprint` Skill 调用本地 stdio MCP server，可连接带认证的 Editor plugin，也可使用随包安装的离线 UAsset 解析器。

## 能力

- 在线或离线检查任意 `.uasset`、`.umap` 的通用序列化结构。
- 专门检查 Blueprint、UMG、AnimBlueprint、AnimMontage、Material、Material Instance 和 Niagara 结构。
- 在线使用 Asset Registry 精确比较与查引用，离线使用序列化结构和二进制引用证据。
- 从 UE Operation Registry 动态取得严格 Schema。
- 预检并自动执行带 `requestId` 幂等保护的事务写入。
- 查询、等待或取消 Job，并独立编译、重载和验证磁盘结构。
- 失败时保留稳定错误码、编译信息和精确的部分失败资产清单。

## 安装

要求 Node.js 22.19+、PowerShell 7、.NET SDK 8+、带 C++ 工具链的 Visual Studio、UE4.27 和 Codex Desktop/CLI：

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

详细信息见[安装](Docs/setup.zh-CN.md)、[架构](Docs/architecture.zh-CN.md)、[MCP 工具](Docs/mcp-reference.zh-CN.md)、[源控恢复](Docs/source-control-recovery.zh-CN.md)和[发布门槛](Docs/v1-release-gate.zh-CN.md)。

English: [README.md](README.md)
