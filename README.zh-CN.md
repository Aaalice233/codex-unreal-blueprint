# pi-unreal-blueprint

> **开发状态：v1.0.0 之前，尚无受支持的正式版本。** 当前源码仍在按证据化发布门槛补齐。不得用于生产资产，也不能因为命令已存在就认定对应 UE4.27 行为已达到发布标准。

[English](README.md) · [安装](Docs/setup.zh-CN.md) · [CLI/Tools](Docs/cli-reference.zh-CN.md) · [产品范围](Docs/product-scope.zh-CN.md) · [架构](Docs/architecture.zh-CN.md) · [手工还原](Docs/source-control-recovery.zh-CN.md) · [CI 与发布](Docs/ci-and-release.zh-CN.md) · [v1.0.0 门槛](Docs/v1-release-gate.zh-CN.md)

## 产品目标

`pi-unreal-blueprint` 计划通过 Pi、稳定 CLI 和 Unreal Editor 插件，为 **Unreal Engine 4.27 Win64** 提供生产级 Blueprint 自动化。所有入口将共用同一套 UE Core 和 Operation Registry。

v1 计划覆盖普通 Blueprint、组件、变量、函数、宏、事件、Event Dispatcher、Interface、Function/Macro Library、Level Blueprint、UMG、AnimBlueprint、UserDefinedStruct 和 UserDefinedEnum。

## 计划中的安全模型

Blueprint 写入会自动执行，不额外弹确认框。因此每个写 Job 必须走同一条严格链路：

1. 锁定准确 Editor 会话并获取写租约；
2. 校验操作、类型、引用、Dirty Package、Source Control 和磁盘状态；
3. 在 Unreal 事务中修改；
4. 编译、保存、重载并验证全部受影响资产；
5. 失败时保留结构化 Job Journal 和准确的 Package 状态清单。

产品不复制 Package、不创建资产备份，也不自动执行 Git/SVN 还原。事务只能撤销保存前的内存修改；部分保存或崩溃后，用户需核对报告清单，再通过现有 Git/SVN 工作副本选择性手工还原。

未知操作或字段、目标歧义、目标 Package 未保存、协议版本不兼容都必须明确失败。断线后客户端必须按原 `requestId` 查询结果，不能盲目重放写请求。

## 计划中的接口

v1 计划提供：

- Pi tools 和 `/unreal-blueprint` 管理界面；
- 同时支持人类输出和 `--json` 的 `pi-unreal-blueprint` CLI；
- 仅监听本机的 JSON-RPC 2.0 通信；
- 共用同一 Core 的 UE4.27 Editor 插件和 Headless Commandlet。

当前源码已包含这些接口的预发布实现。只有 tag 中具备真实 UE4.27 测试证据的能力才受支持；当前调用语法见 [CLI/Tool 参考](Docs/cli-reference.zh-CN.md)。

## v1 边界

- 支持目标：UE4.27、Win64。
- 读写范围：产品范围中列出的完整 Blueprint 体系。
- v1 不包含：UE5、MCP、公共 TypeScript SDK、独立桌面 GUI、UE Dock 面板、Material、Niagara、Sequencer、关卡 Actor 自动化。
- 不采集遥测；操作日志和 Job Journal 仅保留在本机，且不包含资产备份。
- UE 插件不依赖、不链接、不分发 AQ。

## 开发与发布

公开 Pull Request 只在 GitHub-hosted Runner 上运行静态/TypeScript 检查，绝不触达 self-hosted Runner。UE4.27 编译和 E2E 只允许可信 `main`、版本 tag 或受保护环境批准的人工触发执行。

目前没有稳定版本。只有 [v1.0.0 发布门槛](Docs/v1-release-gate.zh-CN.md) 中每一项都有真实 UE4.27 测试证据，才会发布 `v1.0.0`；TODO、占位处理、模拟成功和静默降级均会阻止发布。

从零安装、项目级/引擎级安装、doctor 和本机开发见 [Docs/setup.zh-CN.md](Docs/setup.zh-CN.md)。写入失败后按 [Git/SVN 手工还原说明](Docs/source-control-recovery.zh-CN.md) 操作；产品不会自动恢复。

贡献和安全报告方式见 [CONTRIBUTING.md](CONTRIBUTING.md) 与 [SECURITY.md](SECURITY.md)。

## 许可证

[MIT](LICENSE)
