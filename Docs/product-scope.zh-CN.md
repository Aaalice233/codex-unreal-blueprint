# 产品范围

[English](product-scope.md)

## 状态

本文定义 v1.0.0 的目标范围，不代表这些能力当前已经可用。功能对外发布前必须满足 [发布门槛](v1-release-gate.zh-CN.md) 中的证据要求。

## v1.0.0 目标

- Unreal Engine 4.27 Win64。
- 普通 Blueprint 资产和 Class Defaults。
- SCS 组件、继承覆盖、挂接、Transform、反射属性和资产引用。
- 变量及完整 `FEdGraphPinType`。
- Event、Construction、Function、Macro、Delegate、Interface 和 LevelScript Graph。
- 基于 Action Catalog 的节点、Pin、连线、默认值、注释、Reroute、刷新和确定性布局。
- 函数、宏、自定义事件、Event Dispatcher、Interface、Function Library 和 Macro Library。
- WidgetBlueprint WidgetTree、Slot、属性、绑定、导航、可访问性和 Widget Animation。
- AnimBlueprint AnimGraph、State Machine、Transition、Conduit、Pose Link、变量和 Event Graph。
- UserDefinedStruct 和 UserDefinedEnum。
- 创建、复制、重命名、移动、删除、重设父类、Interface 变更、Redirector 修复、检查、校验、写入、验证和准确的部分失败报告。

每种写操作都必须共用 Operation Registry，并经过完整的预检到验证链路；磁盘状态不明时提供受影响 Package Journal 和 Git/SVN 手工还原指引。只有功能覆盖、没有安全链路，不算满足 v1。

## 明确不做

首版不承诺 UE5、Material、Niagara、Sequencer、关卡 Actor 自动化、MCP、公共 TypeScript SDK、独立 GUI 或 Unreal Dock 面板。Unreal 端只提供状态图标和 Tooltip，详细管理放在 Pi 和 CLI。

产品不维护长期版本化 Plan 文件。Apply 接受当次普通请求，持久化状态仅保留 Job Journal 和审计记录。Journal 不包含资产副本，也不是自动恢复系统。
