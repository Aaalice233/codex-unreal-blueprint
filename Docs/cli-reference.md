# CLI and Tool reference

[简体中文](cli-reference.zh-CN.md)

## Common CLI options

```text
pi-unreal-blueprint <command> [options]

--input <json|@file>   Command parameters as one JSON object
--uproject <path>      Match the exact .uproject session
--session <id>         Select an exact editorSessionId
--timeout <ms>         Timeout from 1 to 600000
--headless             Use the UE4Editor-Cmd Commandlet
--engine-root <path>   UE4.27 root for headless/engine install
--json                 Stable machine-readable envelope
```

Use `@request.json` for non-trivial payloads. Write requests need a unique `requestId`; after a disconnect, query that request/job instead of resubmitting it.

## Commands

| CLI | Pi Tool / behavior |
|---|---|
| `setup` | Runs `scripts/setup.ps1`; accepts `config`, `scope`, `uproject`, `engineRoot`, `pluginTarget`, `piAgentDir`, `piSource`, `skipPiInstall`, `skipUnrealBuild`, and `dryRun` |
| `doctor` | `unreal_doctor` |
| `status` | `unreal_status` |
| `search` | `unreal_search` |
| `capabilities` | `blueprint_capabilities`; authoritative operation schemas |
| `inspect` | `blueprint_inspect` |
| `validate` | `blueprint_validate`; memory-only validation, no persistent Plan |
| `apply` | `blueprint_apply`; starts an automatic write Job |
| `job` | `blueprint_job`; query/wait/cancel at safe phases |
| `verify` | `blueprint_verify` |
| `plugin install/update/remove` | Managed project/engine plugin files |

Examples:

```powershell
pi-unreal-blueprint status --uproject D:/Projects/MyGame/MyGame.uproject --json
pi-unreal-blueprint capabilities --input '{"domain":"graph"}'
pi-unreal-blueprint inspect --uproject D:/Projects/MyGame/MyGame.uproject --input @inspect.json
pi-unreal-blueprint apply --uproject D:/Projects/MyGame/MyGame.uproject --input @apply.json
pi-unreal-blueprint job --uproject D:/Projects/MyGame/MyGame.uproject --input '{"jobId":"...","action":"wait"}'
```

Do not invent operation names or fields. Fetch the current schema through `capabilities`; unknown fields, type mismatches, ambiguous targets, dirty target packages, and incompatible protocol versions fail explicitly.

## Headless

Add `--headless --uproject <path> --engine-root <UE4.27 root>`. The CLI invokes `UE4Editor-Cmd.exe -run=PiUnrealBlueprint` with temporary request/result files and the same UE Core used by the interactive Editor. Headless is not a second editing implementation.

## JSON envelope

`--json` writes a stable top-level envelope:

```json
{"ok":true,"command":"status","result":{}}
```

Failures use `{"ok":false,"error":...}` and a stable non-zero exit code: `1` operation/internal failure, `2` invalid CLI input, `3` setup/plugin management failure, `4` session/connection/protocol failure, and `5` Commandlet launcher failure. A queried result containing `partial` or `stateUnknown` is emitted as failure code `WRITE_PARTIAL` or `WRITE_STATE_UNKNOWN`, with the full result retained in `error.context.details`; it is never printed as success. Scripts must branch on `ok`/exit code and preserve the stable error code and context instead of matching human text.
