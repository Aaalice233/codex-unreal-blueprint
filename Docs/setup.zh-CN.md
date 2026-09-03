# 安装与本地开发

要求 Windows、PowerShell 7、Node.js 22.19+、可实际运行的 Codex CLI、Visual Studio C++、UE4.27 和目标 `.uproject`。

```powershell
npm install
pwsh ./scripts/setup.ps1 -UProject E:/Master/LuaSocial.uproject -EngineRoot E:/UE_4.27 -CodexExecutable C:/path/to/codex.exe
```

`setup.ps1` 会执行 `codex --version`，因此损坏的 npm shim 不会被误判为可用。脚本构建 `dist/mcp/index.js`，构建 Win64 UE plugin，以受管 manifest 同步到项目或 Engine plugin 目录，并把 Codex plugin 安装到个人 Marketplace。重复运行只覆盖受管文件，保留 Marketplace 其他条目和所有非受管文件。

若目标 Editor 正在加载插件，脚本在覆盖任何 UE 文件前失败。关闭 Editor 后重试。可用 `-SkipUnrealBuild` 做不含 C++ 构建的脚本测试，用 `-DryRun` 检查路径和命令计划。

默认配置可复制 `dev.local.example.json` 为未跟踪的 `dev.local.json`。安装后重启 Editor 并新建 Codex task。

English: [setup.md](setup.md)
