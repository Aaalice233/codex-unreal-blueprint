# PiUnrealBlueprint UE4.27 Plugin

## 中文

这是仅面向 UE4.27 Editor/Commandlet 的插件，不进入 Runtime，也不依赖 AQ。

模块边界：

- `PiUnrealBlueprintCore`：统一实现状态、诊断、搜索、能力查询、检查、校验、写入 Job 和独立验证。
- `PiUnrealBlueprintTransport`：提供 localhost TCP JSON-RPC 通信、认证、会话描述和长度前缀分帧。
- `PiUnrealBlueprintEditor`：提供 UE4.27 Level Editor 状态栏图标和 Tooltip。
- `PiUnrealBlueprintCommandlet`：从请求文件调用同一 Core，并把结构化结果写入结果文件。
- `PiUnrealBlueprintTests`：提供协议、Transport、能力和写入流水线的 UE Automation Tests。

已接入的方法包括 `unreal.status`、`unreal.doctor`、`unreal.search`、`blueprint.capabilities`、`blueprint.inspect`、`blueprint.validate`、`blueprint.apply`、`blueprint.job` 和 `blueprint.verify`。实际可发布能力仍以 tag 中通过的 UE4.27 测试和仓库根目录 `Docs/v1-release-gate.zh-CN.md` 为准。

Commandlet 形式：

```text
UE4Editor-Cmd.exe Project.uproject -run=PiUnrealBlueprint -Request="request.json" -Result="result.json"
```

## English

This UE4.27 Editor/Commandlet-only plugin is excluded from runtime targets and has no AQ dependency. The interactive Editor transport and Commandlet share the same Core implementation.

Implemented method boundaries include `unreal.status`, `unreal.doctor`, `unreal.search`, `blueprint.capabilities`, `blueprint.inspect`, `blueprint.validate`, `blueprint.apply`, `blueprint.job`, and `blueprint.verify`. Release support is limited to capabilities backed by UE4.27 test evidence in the tagged commit and the repository release gate.
