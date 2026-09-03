# Setup and local development

Requires Windows, PowerShell 7, Node.js 22.19+, .NET SDK 8+, a working Codex CLI, Visual Studio C++ tools, UE4.27, and a target `.uproject`. The .NET SDK builds the bundled offline UAsset parser on first use; its NuGet packages are then reused from the local cache.

```powershell
npm install
pwsh ./scripts/setup.ps1 -UProject E:/Master/LuaSocial.uproject -EngineRoot E:/UE_4.27 -CodexExecutable C:/path/to/codex.exe
```

`setup.ps1` executes `codex --version`, so a broken npm shim is not accepted. It builds `dist/mcp/index.js`, builds the Win64 UE plugin, syncs the DLLs from that validated build together with source files through a managed manifest, and installs the Codex plugin in the personal Marketplace. Repeated runs only replace managed files, preserve other Marketplace entries, and retain all unmanaged files.

Each install gives the installed Codex plugin copy a fresh `+codex.<timestamp>` cachebuster without changing the repository release version. This avoids overwriting an in-use cache; restart Codex and open a new task to load the update.

If the target Editor is loading the plugin, setup fails before overwriting UE files. Close the Editor and retry. Use `-SkipUnrealBuild` for isolated installer tests and `-DryRun` to inspect resolved paths and commands.

Copy `dev.local.example.json` to the ignored `dev.local.json` for local defaults. Restart the Editor and open a new Codex task after installation.

中文：[setup.zh-CN.md](setup.zh-CN.md)
