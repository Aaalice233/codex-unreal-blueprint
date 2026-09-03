# CLI 与 Tool 参考

[English](cli-reference.md)

## 通用 CLI 参数

```text
pi-unreal-blueprint <command> [options]

--input <json|@file>   以 JSON object 传入命令参数
--uproject <path>      精确匹配 .uproject 会话
--session <id>         精确选择 editorSessionId
--timeout <ms>         1 到 600000 毫秒超时
--headless             使用 UE4Editor-Cmd Commandlet
--engine-root <path>   Headless/引擎级安装使用的 UE4.27 根目录
--json                 稳定的机器可读 envelope
```

复杂请求使用 `@request.json`。写请求必须带唯一 `requestId`；断线后查询原请求/Job，不能重新提交。

## 命令

| CLI | Pi Tool / 行为 |
|---|---|
| `setup` | 运行 `scripts/setup.ps1`；接受 `config`、`scope`、`uproject`、`engineRoot`、`pluginTarget`、`piAgentDir`、`piSource`、`skipPiInstall`、`skipUnrealBuild`、`dryRun` |
| `doctor` | `unreal_doctor` |
| `status` | `unreal_status` |
| `search` | `unreal_search` |
| `capabilities` | `blueprint_capabilities`；operation schema 权威来源 |
| `inspect` | `blueprint_inspect` |
| `validate` | `blueprint_validate`；只在内存校验，不创建持久 Plan |
| `apply` | `blueprint_apply`；启动自动写入 Job |
| `job` | `blueprint_job`；查询、等待或在安全阶段取消 |
| `verify` | `blueprint_verify` |
| `plugin install/update/remove` | 管理项目级/引擎级插件文件 |

示例：

```powershell
pi-unreal-blueprint status --uproject D:/Projects/MyGame/MyGame.uproject --json
pi-unreal-blueprint capabilities --input '{"domain":"graph"}'
pi-unreal-blueprint inspect --uproject D:/Projects/MyGame/MyGame.uproject --input @inspect.json
pi-unreal-blueprint apply --uproject D:/Projects/MyGame/MyGame.uproject --input @apply.json
pi-unreal-blueprint job --uproject D:/Projects/MyGame/MyGame.uproject --input '{"jobId":"...","action":"wait"}'
```

不得猜 operation 名称或字段。先通过 `capabilities` 获取当前 schema；未知字段、类型不匹配、目标歧义、目标 Package 为 Dirty、协议版本不兼容都会明确失败。

## Headless

增加 `--headless --uproject <path> --engine-root <UE4.27 root>`。CLI 通过临时请求/结果文件调用 `UE4Editor-Cmd.exe -run=PiUnrealBlueprint`，并与交互 Editor 共用同一 UE Core，不维护第二套编辑逻辑。

## JSON envelope

`--json` 输出稳定顶层 envelope：

```json
{"ok":true,"command":"status","result":{}}
```

失败输出 `{"ok":false,"error":...}`，退出码固定为：`1` 操作/内部失败、`2` CLI 参数错误、`3` setup/插件管理失败、`4` 会话/连接/协议失败、`5` Commandlet launcher 失败。查询结果只要包含 `partial` 或 `stateUnknown`，就会按 `WRITE_PARTIAL` 或 `WRITE_STATE_UNKNOWN` 失败输出，完整结果保留在 `error.context.details`，不会显示成成功。脚本应判断 `ok`/退出码并保留稳定错误码和上下文，不要匹配人类可读文本。
