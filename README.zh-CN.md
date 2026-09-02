# pi-unreal-blueprint

> **开发状态：v1.0.0 之前，当前不可用。** 仓库目前包含早期 TypeScript、UE 插件骨架以及文档/CI 基线；只有有限的 `unreal_status` 骨架。下文所述 Blueprint 检查、校验、写入、Job、恢复、通信、setup 和发布能力仍在开发中。请勿把当前仓库当作可用的 Blueprint 自动化工具安装。

[English](README.md) · [产品范围](Docs/product-scope.zh-CN.md) · [架构](Docs/architecture.zh-CN.md) · [CI 与发布](Docs/ci-and-release.zh-CN.md) · [v1.0.0 门槛](Docs/v1-release-gate.zh-CN.md)

## 产品目标

`pi-unreal-blueprint` 计划通过 Pi、稳定 CLI 和 Unreal Editor 插件，为 **Unreal Engine 4.27 Win64** 提供生产级 Blueprint 自动化。所有入口将共用同一套 UE Core 和 Operation Registry。

v1 计划覆盖普通 Blueprint、组件、变量、函数、宏、事件、Event Dispatcher、Interface、Function/Macro Library、Level Blueprint、UMG、AnimBlueprint、UserDefinedStruct 和 UserDefinedEnum。

## 计划中的安全模型

Blueprint 写入会自动执行，不额外弹确认框。因此每个写 Job 必须先经过严格预检，并作为一个可恢复整体执行：

1. 锁定准确 Editor 会话并获取写租约；
2. 校验操作、类型、引用、Dirty Package、Source Control 和磁盘状态；
3. 备份完整受影响 Package 集合；
4. 在 Unreal 事务中修改；
5. 编译、保存、重载并验证全部受影响资产；
6. 任一步失败则整批恢复，并保留结构化 Job Journal。

未知操作或字段、目标歧义、目标 Package 未保存、协议版本不兼容都必须明确失败。断线后客户端必须按原 `requestId` 查询结果，不能盲目重放写请求。

## 计划中的接口

v1 计划提供：

- Pi tools 和 `/unreal-blueprint` 管理界面；
- 同时支持人类输出和 `--json` 的 `pi-unreal-blueprint` CLI；
- 仅监听本机的 JSON-RPC 2.0 通信；
- 共用同一 Core 的 UE4.27 Editor 插件和 Headless Commandlet。

这些接口**目前尚不可用**。只有对应实现和测试落地后，文档才会加入可执行命令示例。

## v1 边界

- 支持目标：UE4.27、Win64。
- 读写范围：产品范围中列出的完整 Blueprint 体系。
- v1 不包含：UE5、MCP、公共 TypeScript SDK、独立桌面 GUI、UE Dock 面板、Material、Niagara、Sequencer、关卡 Actor 自动化。
- 不采集遥测；操作日志和恢复数据仅保留在本机。
- UE 插件不依赖、不链接、不分发 AQ。

## 开发与发布

公开 Pull Request 只在 GitHub-hosted Runner 上运行静态/TypeScript 检查，绝不触达 self-hosted Runner。UE4.27 编译和 E2E 只允许可信 `main`、版本 tag 或受保护环境批准的人工触发执行。

目前没有稳定版本。只有 [v1.0.0 发布门槛](Docs/v1-release-gate.zh-CN.md) 中每一项都有真实 UE4.27 测试证据，才会发布 `v1.0.0`；TODO、占位处理、模拟成功和静默降级均会阻止发布。

贡献和安全报告方式见 [CONTRIBUTING.md](CONTRIBUTING.md) 与 [SECURITY.md](SECURITY.md)。

## 许可证

[MIT](LICENSE)
