# 安装与本机开发

[English](setup.md)

## 前置条件

- Windows 10/11 x64
- UE4.27，且具备 `RunUAT.bat`、`UE4Editor.exe`、`UE4Editor-Cmd.exe`
- 安装 UE4.27 所需 MSVC x64 C++ 工作负载的 Visual Studio
- Node.js 22.19 或更高版本、npm、PowerShell 7、Pi
- 一个可写的 UE4.27 `.uproject`

v1 不支持 UE5。

## 从 Release 全新安装

`v1.0.0` 发布后，先创建下方本机配置，再运行锁定版本的 package CLI：

```powershell
npx --yes pi-unreal-blueprint@1.0.0 setup --input '{"config":"C:/work/pi-unreal-blueprint.local.json"}'
```

默认且推荐项目级安装：

```json
{
  "piAgentDir": "C:/Users/me/.pi/agent",
  "uproject": "D:/Projects/MyGame/MyGame.uproject",
  "engineRoot": "C:/Program Files/Epic Games/UE_4.27",
  "installScope": "project"
}
```

`npx` 直接运行已发布 CLI，不假设 Pi 的 package 目录已加入 `PATH`。在 npm package 模式下，`setup` 会执行 `pi install npm:pi-unreal-blueprint@1.0.0` 安装同版本 Pi package，使用 package 内预构建 CLI 而不运行 npm 包中不存在的源码开发测试，再构建 UE4.27 插件、只镜像受管文件并运行安装 doctor。只有 Pi 已安装完全相同版本时才设置 `skipPiInstall: true`。完成后重启 Pi（或执行 `/reload`），并重启 Unreal Editor。

需要引擎级安装时设置 `"installScope": "engine"`，目标为 `<EngineRoot>/Engine/Plugins/Developer/PiUnrealBlueprint`。引擎级安装是可选项，会影响该引擎下的所有项目。显式 `uePluginTarget` 会覆盖推导目标，但目录名必须是 `PiUnrealBlueprint`。

源码 checkout 会从本地仓库安装 Pi package，并在构建、同步前运行仓库 package 检查：

```powershell
git clone https://github.com/Aaalice233/pi-unreal-blueprint.git
Set-Location pi-unreal-blueprint
Copy-Item dev.local.example.json dev.local.json
# 修改本机路径后执行：
./scripts/setup.ps1 -Config ./dev.local.json
```

`dev.local.json` 已被 Git 忽略，不得提交私有项目或引擎路径。

## Doctor

遇到以下情况时 setup 会明确失败，不会静默继续：缺少 Node/Pi/MSVC/UE 路径、插件目标不安全、CLI 未构建、受管 manifest 不存在、镜像文件哈希不一致。Editor 启动后执行：

```powershell
pi-unreal-blueprint doctor --uproject D:/Projects/MyGame/MyGame.uproject
pi-unreal-blueprint status --uproject D:/Projects/MyGame/MyGame.uproject
```

Doctor 应报告 package/插件/协议兼容性、准确项目会话、UE4.27、本机通信、权限、构建工具和插件安装状态。多个 Editor 同时运行时，传入 `--session <editorSessionId>`，不能依赖发现顺序。

## 插件管理

```powershell
pi-unreal-blueprint plugin install --uproject D:/Projects/MyGame/MyGame.uproject
pi-unreal-blueprint plugin update --uproject D:/Projects/MyGame/MyGame.uproject
pi-unreal-blueprint plugin remove --uproject D:/Projects/MyGame/MyGame.uproject
```

引擎级操作需增加 `--engine-root <path>` 和 `--input '{"scope":"engine"}'`。安装、更新和移除只处理 `.pi-unreal-blueprint.manifest.json` 记录的文件；其他文件和目录保留。遇到未受管同名文件时直接停止。

## 本机开发

```powershell
./scripts/dev.ps1 check
./scripts/dev.ps1 sync
./scripts/dev.ps1 publish -Message "feat(graph): 添加节点动作目录"
```

- `check` 运行 package 检查、独立目录中的 `RunUAT BuildPlugin` 和可选 UE 测试。
- `sync` 先检查；目标 Editor 正在运行且插件有变化时，在覆盖前停止；否则只镜像受管文件。
- `publish` 校验提交消息和仓库状态，检查后提交并推送。它不会 force push、reset、clean 或 stash。Editor 正在运行只延后本机插件同步，不阻止 Git push。

通过 `-RunUnrealTests` 或 `"runUeTests": true` 开启 UE Automation 测试。生成产物和本机配置保持忽略。
