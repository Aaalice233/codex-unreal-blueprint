# 架构基线

[English](architecture.md)

> 以下是计划架构，实现仍在进行中。

```text
Pi Extension / CLI
        │ 本机 TCP、长度前缀 JSON-RPC 2.0
PiUnrealBlueprint Editor Plugin
        │
        ├─ Core + Operation Registry
        ├─ Transport 和会话发现
        ├─ 状态图标 / Tooltip
        └─ Commandlet（共用 Core）
```

## 唯一事实来源

- `PLAN.md` 是产品范围和完成标准的权威依据。
- UE Core Operation Registry 将作为操作名和参数 JSON Schema 的唯一事实来源。
- Pi tools、CLI、交互 Editor 和 Commandlet 禁止复制第二套编辑逻辑。

## 会话与通信

Editor 将监听随机 `127.0.0.1` 端口，并写入仅当前用户可读的会话描述，其中包含项目路径、PID、版本、能力摘要、端口和每次启动生成的 token。Pi 按规范化 `.uproject` 路径选择；存在多个候选时必须明确选择。

帧大小、嵌套深度和批量数量均受限。全部 UObject 操作回到 Game Thread。读取可并行，写入使用带心跳的单写租约。每次写入都有唯一 `requestId` 和持久化结果状态。

## 写入不变量

只有完整受影响 Package 集合依次通过预检、备份、事务、修改、结构回读、编译、保存、重载和验证，写入才算成功。保存前失败回滚内存；部分保存、崩溃或连接状态不明时，按 Journal 整批恢复。若 Job 后资产又被用户修改，恢复必须停止，不能覆盖新改动。

## 公共接口基线

计划中的高层 tools 为 `unreal_status`、`unreal_doctor`、`unreal_search`、`blueprint_capabilities`、`blueprint_inspect`、`blueprint_validate`、`blueprint_apply`、`blueprint_job`、`blueprint_verify` 和 `blueprint_history`。具体 operation schema 仅在需要时从 Registry 获取。
