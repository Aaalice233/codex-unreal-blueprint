# CodexUnrealBlueprint

UE4.27 Editor-only Blueprint automation plugin for Codex. It contains four modules:

- `CodexUnrealBlueprintCore`: Operation Registry, inspection, validation, writes, jobs, persistence, and verification.
- `CodexUnrealBlueprintTransport`: authenticated localhost JSON-RPC session transport.
- `CodexUnrealBlueprintEditor`: status-bar icon and tooltip.
- `CodexUnrealBlueprintTests`: Editor Automation tests.

The plugin does not enter runtime targets and has no external Unreal dependency. Enable it in the target `.uproject`, restart the Editor, and connect through the bundled Codex MCP server.
