# MCP tool reference

Every tool accepts optional `session: { editorSessionId?, uproject? }`. An exact `editorSessionId` is required when multiple Editors match. Fetch operation fields from `blueprint_capabilities`; this document intentionally does not duplicate the dynamic registry schema.

| Tool | Purpose | Annotation |
|---|---|---|
| `unreal_status` | Session, PIE, source-control, dirty-package, and queue status | read-only |
| `unreal_doctor` | Plugin, protocol, project, port, permission, and build diagnostics | read-only |
| `unreal_search` | Search assets, classes, members, properties, actions, or operations | read-only |
| `blueprint_capabilities` | Read schemas and examples from the Operation Registry | read-only |
| `blueprint_inspect` | Page facets, stable IDs, compile state, and structure hashes | read-only |
| `blueprint_validate` | Preflight one-shot operations in memory | read-only |
| `blueprint_apply` | Start an automatic transactional write with a unique `requestId` | destructive |
| `blueprint_job` | Query, wait for, or cancel by `jobId`/`requestId` | non-read-only |
| `blueprint_verify` | Compile, reload, and assert disk structure | read-only |

`blueprint_job wait` accepts `timeoutMs` from 0 through 600000; the MCP host timeout is 620 seconds. Successful results appear in both text and `structuredContent.result`. Failures use `structuredContent.error` with stable codes and available asset, operation, callsite, compiler, and partial-failure details.

中文：[mcp-reference.zh-CN.md](mcp-reference.zh-CN.md)
