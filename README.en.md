<p align="center">
  <img src="Docs/images/readme-banner.png" alt="Codex Unreal Blueprint banner" width="100%">
</p>

# Codex Unreal Blueprint

[简体中文](README.md) · [繁體中文](README.zh-TW.md) · **English**

A local UE4.27 Win64 plugin for Codex. It safely automates Blueprints while the Editor is running and inspects `.uasset` and `.umap` files when the Editor is closed. The Skill, MCP server, UE Editor plugin, and offline parser ship together—no separate `inspect-unreal-uassets` skill is required.

## ✨ Highlights

- Inspect Unreal assets online or offline with explicit `generic`, `specialized`, and `editable` capability layers.
- Create, duplicate, move, rename, edit, and delete common Blueprint assets.
- Edit components, variables, graphs, nodes, pins, links, UMG WidgetTrees, and AnimBlueprints.
- Read Blueprint, UMG, AnimBlueprint, AnimMontage, Material, Material Instance, and Niagara System data in depth.
- Compare assets and find dependencies or referencers using Asset Registry data online and serialized evidence offline.
- Preflight every write, then compile, save, reload, and verify it while preserving exact failures and compiler output.

## 🧭 Online, offline, and automatic routing

| Mode | Best for | Behavior |
|---|---|---|
| `auto` | Everyday use; recommended | Uses the unique matching Editor when available, otherwise falls back to a supplied file path |
| `editor` | Unsaved state, precise references, Blueprint writes | Connects to the authenticated UE Editor plugin and reads live UObject and Asset Registry data |
| `offline` | A closed Editor, disk evidence, or assets better suited to static parsing | Uses the bundled UAssetAPI parser; always read-only and never presented as runtime truth |

For effects or other packages that may be held by the Editor, enable isolated staging:

```json
{
  "mode": "offline",
  "filePath": "E:/Project/Content/Effects/NS_Test.uasset",
  "contentRoot": "E:/Project/Content",
  "offlineStaging": {
    "enabled": true,
    "maxCachedAssets": 64
  }
}
```

The parser copies the requested package and any `.uexp`, `.ubulk`, or `.uptnl` companions, verifies that each source remained stable, and parses the isolated copy. Only primary `.uasset/.umap` files consume cache slots. Once the limit is reached, the oldest snapshots are evicted and new work continues; historical cache usage never blocks a new asset.

## 🧩 Asset support

| Asset type | Specialized inspection | Online editing |
|---|---:|---:|
| Blueprint, Actor, ActorComponent, Interface, Function/Macro Library | ✅ | ✅ |
| UMG / Widget Blueprint | ✅ WidgetTree, graphs, animation, properties | ✅ Existing assets |
| Animation Blueprint | ✅ AnimGraph, state machines, variables | ✅ Existing assets |
| User Defined Struct / Enum and Level Blueprint | ✅ | ✅ |
| AnimMontage | ✅ Sections, slots, notifies, blends | Read-only |
| Material / Material Instance | ✅ Parameters, parent, expressions | Read-only |
| Niagara System | ✅ Parameters, emitters, warmup, serialized evidence | Read-only |
| Other loadable assets such as Texture, Mesh, Sound, DataAsset, and Sequence | Generic properties, dependencies, referencers, imports/exports | Read-only |

The creation API currently exposes Blueprint, Interface, Function Library, Macro Library, Struct, and Enum. Existing UMG and AnimBlueprint assets can be edited in depth; dedicated creation entry points are not exposed yet.

## 🛠️ Blueprint automation

- **Assets:** create, duplicate, move, rename, delete, reparent, manage interfaces, and set class defaults.
- **Components:** add, remove, rename, attach, set the root, transform, properties, and inherited overrides.
- **Types:** add, update, or remove variables; edit Struct fields and Enum values.
- **Graphs:** create or remove graphs; edit signatures, locals, dispatchers, nodes, pins, and links.
- **UMG:** edit widget hierarchy, named slots, properties, events, bindings, navigation, accessibility, and timeline animation.
- **AnimBlueprint:** edit skeleton and parent, AnimGraph nodes, state machines, states, conduits, transitions, pose links, and Event Graphs.

Operation names and parameter schemas come directly from the running UE plugin's Operation Registry. The Skill does not guess unsupported nodes or fields.

## 🛡️ Write safety

Every write requires a unique `requestId` and follows the same pipeline:

```text
strict preflight → UE transaction → modify → compile → save → reload → structural verification
```

- Dirty target packages, source-control checkout failures, schema mismatches, and compiler errors stop explicitly.
- If the connection becomes uncertain, the original `requestId` is queried instead of replaying the write.
- Partial failures report exact `modified`, `saved`, `notSaved`, and `unknown` asset lists.
- The plugin does not back up or restore binary assets automatically; users retain control of Git/SVN recovery.

## 🚀 Installation

Requirements: Windows, UE4.27, PowerShell 7, Node.js 22.19+, .NET SDK 8+, Visual Studio C++ tools, and Codex Desktop/CLI.

For the first installation or a UE plugin update, close the target Editor and run:

```powershell
npm install
pwsh ./scripts/setup.ps1 `
  -UProject E:/Project/MyGame.uproject `
  -EngineRoot E:/UE_4.27
```

The installer runs checks, builds the UE4.27 Win64 plugin, synchronizes managed files, and installs the personal Codex plugin. Restart the Editor and create a new Codex task afterward.

If the UE plugin is already current and only the Skill, MCP server, or offline parser changed, keep the Editor open and run:

```powershell
pwsh ./scripts/setup.ps1 -CodexOnly
```

No Editor restart is needed in this case; create a new Codex task to load the update. Add `-CodexExecutable C:/path/to/codex.exe` only if automatic CLI discovery fails.

## 💬 Example requests

- “Inspect this UMG's WidgetTree and button events.”
- “Compare these Niagara systems and show parameter and emitter differences.”
- “Add a component, variables, and initialization nodes to this Actor Blueprint, then verify it.”
- “Keep the Editor open and parse this effect from an offline staged copy.”
- “Find everything that references this Blueprint and distinguish online from offline evidence.”

## 🧰 MCP tools

- **Environment and search:** `unreal_status`, `unreal_doctor`, `unreal_search`
- **General assets:** `unreal_asset_inspect`, `unreal_asset_compare`, `unreal_asset_referencers`
- **Blueprint workflow:** `blueprint_capabilities`, `blueprint_inspect`, `blueprint_validate`, `blueprint_apply`, `blueprint_job`, `blueprint_verify`

## 📚 Documentation

- [Setup and local development](Docs/setup.md)
- [Architecture and capability layers](Docs/architecture.md)
- [MCP tool reference](Docs/mcp-reference.md)
- [Manual Git/SVN recovery](Docs/source-control-recovery.md)
- [v1.0.0 release gate](Docs/v1-release-gate.md)

## ⚠️ Boundaries

- Offline output is serialized disk evidence, not proof of final state after Construction Script, Lua/C++, or runtime changes.
- Cooked, unversioned, damaged, or heavily customized packages may parse only partially.
- Material, Niagara, AnimMontage, and other non-Blueprint assets are currently for inspection, comparison, and reference analysis—not writes.
- The supported target is UE4.27 Win64; compatibility with other engine versions is not claimed.

## 🤝 Development and license

```powershell
npm run check
```

Read [CONTRIBUTING.md](CONTRIBUTING.md) and [SECURITY.md](SECURITY.md) before contributing. Licensed under the [MIT License](LICENSE); bundled parser notices are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
