# CodexUnrealBlueprint

UE4.27 Editor-only layered asset inspection and Blueprint automation plugin for Codex. It contains four modules:

- `CodexUnrealBlueprintCore`: generic/specialized asset inspection, Operation Registry, validation, writes, jobs, persistence, and verification.
- `CodexUnrealBlueprintTransport`: authenticated localhost JSON-RPC session transport.
- `CodexUnrealBlueprintEditor`: status-bar icon and tooltip.
- `CodexUnrealBlueprintTests`: Editor Automation tests.

The plugin does not enter runtime targets. Its Niagara specialization uses UE4.27's built-in Niagara plugin. Enable it in the target `.uproject`, restart the Editor, and connect through the bundled Codex MCP server.
