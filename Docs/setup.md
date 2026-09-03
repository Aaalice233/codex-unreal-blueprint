# Setup and local development

[简体中文](setup.zh-CN.md)

## Requirements

- Windows 10/11 x64
- Unreal Engine 4.27 with `RunUAT.bat`, `UE4Editor.exe`, and `UE4Editor-Cmd.exe`
- Visual Studio with the MSVC x64 C++ workload required by UE4.27
- Node.js 22.19 or newer, npm, PowerShell 7, and Pi
- A writable UE4.27 `.uproject`

UE5 is not supported by v1.

## Fresh install from a release

After `v1.0.0` is published, create the local configuration below and run the version-pinned package CLI:

```powershell
npx --yes pi-unreal-blueprint@1.0.0 setup --input '{"config":"C:/work/pi-unreal-blueprint.local.json"}'
```

Project scope is the default and is recommended:

```json
{
  "piAgentDir": "C:/Users/me/.pi/agent",
  "uproject": "D:/Projects/MyGame/MyGame.uproject",
  "engineRoot": "C:/Program Files/Epic Games/UE_4.27",
  "installScope": "project"
}
```

`npx` runs the published CLI without assuming that Pi's package directory is on `PATH`. In npm-package mode, `setup` installs the same pinned package into Pi with `pi install npm:pi-unreal-blueprint@1.0.0`, uses the packaged prebuilt CLI instead of unavailable source-only development tests, builds the UE4.27 plugin, mirrors only package-managed plugin files, and runs the installation doctor. Set `skipPiInstall` to `true` only when that exact Pi package is already installed. Restart Pi (or run `/reload`) and restart Unreal Editor after installation.

For an engine-level installation, set `"installScope": "engine"`. The target becomes `<EngineRoot>/Engine/Plugins/Developer/PiUnrealBlueprint`. Engine scope is optional and affects every project using that engine installation. An explicit `uePluginTarget` overrides the derived target and must end in `PiUnrealBlueprint`.

A source checkout installs Pi from that local checkout and runs the repository's package checks before building and syncing:

```powershell
git clone https://github.com/Aaalice233/pi-unreal-blueprint.git
Set-Location pi-unreal-blueprint
Copy-Item dev.local.example.json dev.local.json
# Edit machine-local paths, then:
./scripts/setup.ps1 -Config ./dev.local.json
```

`dev.local.json` is ignored by Git. Do not commit private project or engine paths.

## Doctor

Setup fails rather than silently continuing when Node/Pi/MSVC/UE paths are missing, the plugin target is unsafe, generated CLI output is absent, the managed manifest is absent, or a mirrored file hash differs. After the Editor starts, run:

```powershell
pi-unreal-blueprint doctor --uproject D:/Projects/MyGame/MyGame.uproject
pi-unreal-blueprint status --uproject D:/Projects/MyGame/MyGame.uproject
```

Doctor should report package/plugin/protocol compatibility, exact project session, UE4.27, localhost transport, permissions, build tools, and plugin installation. If multiple Editors are open, pass `--session <editorSessionId>` instead of relying on discovery order.

## Plugin management

```powershell
pi-unreal-blueprint plugin install --uproject D:/Projects/MyGame/MyGame.uproject
pi-unreal-blueprint plugin update --uproject D:/Projects/MyGame/MyGame.uproject
pi-unreal-blueprint plugin remove --uproject D:/Projects/MyGame/MyGame.uproject
```

For engine scope, add `--engine-root <path>` and `--input '{"scope":"engine"}'`. Install/update/remove only touch files listed in `.pi-unreal-blueprint.manifest.json`; unmanaged files and directories are preserved. A conflicting unmanaged file stops the operation.

## Local development

```powershell
./scripts/dev.ps1 check
./scripts/dev.ps1 sync
./scripts/dev.ps1 publish -Message "feat(graph): 添加节点动作目录"
```

- `check` runs package checks, a staged `RunUAT BuildPlugin`, and optional UE tests.
- `sync` checks first, refuses to overwrite loaded plugin binaries while the target Editor is running, then mirrors managed files.
- `publish` validates the commit message and repository state, checks, commits, and pushes. It never force-pushes, resets, cleans, or stashes. A running Editor delays only local plugin sync, not the Git push.

Use `-RunUnrealTests` or `"runUeTests": true` for UE Automation tests. Generated artifacts and local configuration remain ignored.
