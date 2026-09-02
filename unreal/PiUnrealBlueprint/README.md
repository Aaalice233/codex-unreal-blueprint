# PiUnrealBlueprint UE4.27 Plugin Skeleton

## 中文

这是仅面向 UE4.27 Editor/Commandlet 的插件骨架，不进入 Runtime，也不依赖 AQ。

模块边界：

- `PiUnrealBlueprintCore`：协议、错误、状态和统一请求分发入口。
- `PiUnrealBlueprintTransport`：长度前缀帧编解码；TCP 服务尚未实现，会明确报告 `NotImplemented`。
- `PiUnrealBlueprintEditor`：UE4.27 Level Editor 状态栏集成，显示当前不可用原因。
- `PiUnrealBlueprintCommandlet`：读取请求文件并调用 Core；未支持的方法返回结构化 `NotImplemented`。
- `PiUnrealBlueprintTests`：协议、分帧和未实现错误的 Automation Tests。

当前唯一可执行方法是 `unreal_status`。Blueprint 读取、校验、写入、Job、恢复和 TCP 监听均未实现，不会返回模拟成功。

Commandlet 形式：

```text
UE4Editor-Cmd.exe Project.uproject -run=PiUnrealBlueprint -Request="request.json" -Result="result.json"
```

## English

This is an UE4.27 Editor/Commandlet-only plugin skeleton. It is excluded from runtime targets and has no AQ dependency.

Only `unreal_status` is implemented. Blueprint inspection, validation, mutation, jobs, recovery, and TCP listening return explicit structured `NotImplemented` errors rather than simulated success.
