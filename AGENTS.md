# pi-unreal-blueprint 开发规范

## 权威范围

- `PLAN.md` 是产品范围、架构、接口、安全、测试和 v1.0.0 完成标准的唯一权威依据。
- 第一版必须完整交付 `PLAN.md` 的生产级能力，不得用 TODO、占位接口、模拟成功或静默降级冒充完成。
- 首版仅支持 UE4.27 Win64 的完整 Blueprint 体系，不扩展到 Material、Niagara、Sequencer 或关卡 Actor 自动化。

## 已确认的产品行为

- Blueprint 写入默认完全自动执行，不增加计划确认弹窗。
- 自动写入必须同时具备前置校验、事务、整批资产备份、操作日志、编译、保存、重载验证和失败恢复。
- 本机 UE 集成测试默认使用 `E:/Master/LuaSocial.uproject`；产品开发任务获准自动操作 E:/Master 资产。
- 自动测试默认在 `/Game/PiAutomation/<runId>` 创建可清理 Fixture，除非测试明确指定其他目标。
- UE 插件只提供状态栏图标和 Tooltip；详细管理集中在 Pi 面板和 CLI。
- 不引入需要长期维护的版本化 Plan 文件；批处理只接受当次普通 JSON 请求，持久化仅保留 Job Journal 和恢复审计记录。
- UE 插件不依赖、不链接、不分发 AQ；所有产品能力使用 UE4.27 原生 Editor C++ API 独立实现。

## 工程规则

- 面向用户的文档和交互提供中英双语；代码、API、协议字段和专有名词保持英文。
- 修改前检查目标文件、调用关系和现有改动；不覆盖、回滚或格式化无关内容。
- Pi Extension、CLI 和 Commandlet 必须调用同一 Operation Registry 与 UE Core，禁止维护第二套编辑逻辑。
- Operation Registry 是操作参数 Schema 的唯一事实来源；未知字段、未知操作和类型不匹配必须明确失败。
- 写请求必须携带 `requestId`；连接不明时查询原请求，禁止盲目重放。
- 不吞异常、不返回假成功；错误必须包含稳定错误码、资产路径、operation 索引、UE 调用位置和原始编译信息。
- Git 提交消息使用 `type(scope): 中文描述`，不超过 72 字；禁止交互式提交编辑器、force push、reset、clean 和 stash。
- Git 提交和云端推送必须与本机 UE 插件同步解耦；Editor 运行中只阻止或延后插件覆盖，不得阻止提交或推送。

## 验证要求

- 先运行针对性 TS/C++ 测试，再按风险运行 UE4.27 E2E、持久化和故障注入测试。
- 每类写能力都必须验证：预检、备份、事务、修改、编译、保存、重载、结构断言和恢复。
- 交互 Editor 与 Headless Commandlet 对同一请求必须得到等价结构结果。
- 发布前必须复查 diff，排除重复事实来源、宽泛吞错、部分成功、死代码和未说明的行为变化。
