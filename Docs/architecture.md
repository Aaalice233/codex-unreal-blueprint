# Architecture

```text
Codex task
  ├─ skills/unreal-blueprint/SKILL.md
  └─ dist/mcp/index.js (stdio)
          └─ src/client (TCP JSON-RPC 2.0)
                  └─ CodexUnrealBlueprint UE4.27 Editor plugin
                         ├─ Core / Operation Registry
                         ├─ Transport
                         ├─ Editor status
                         └─ Tests
```

MCP owns fixed tool envelopes, Codex annotations, session selection, and error serialization. Operation schemas and execution exist only in the UE Operation Registry. The Editor binds a random `127.0.0.1` port and publishes a current-user-only descriptor with a per-start token. Clients select by canonical `.uproject` or exact `editorSessionId`.

Writes use a `requestId` journal. On connection loss the client queries that request instead of replaying it. Core performs preflight, transaction, mutation, compilation, save, reload verification, and failure asset classification within one write job.

中文：[architecture.zh-CN.md](architecture.zh-CN.md)
