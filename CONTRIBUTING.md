# Contributing / 贡献指南

## English

Thank you for helping build `pi-unreal-blueprint`. The project is pre-v1 and follows `PLAN.md` as the authoritative product and completion specification.

### Before opening a change

1. Read `AGENTS.md`, `PLAN.md`, the [product scope](Docs/product-scope.md), and [architecture](Docs/architecture.md).
2. Keep v1 limited to UE4.27 Win64 and the Blueprint system. Propose unrelated domains separately; do not silently expand scope.
3. Open an issue before changing a public protocol, Operation Registry contract, write/recovery invariant, or release trust boundary.
4. Never describe planned behavior as implemented. Update both English and Chinese user-facing documentation when behavior changes.

### Engineering requirements

- Pi Extension, CLI, Editor, and Commandlet must share one UE Core and Operation Registry.
- Unknown operations/fields, incompatible types, ambiguity, and failures must return explicit stable errors; no mock success or silent fallback.
- A write is incomplete until backup, transaction, compile, save, reload, verification, journal, and applicable recovery tests pass.
- Keep public Fork code on GitHub-hosted runners. Changes that route `pull_request` code to self-hosted infrastructure will not be accepted.
- Do not commit credentials, session tokens, local project paths/configuration, Unreal assets from private projects, generated plugin binaries, backups, or Job Journals.

### Pull requests

Use a focused branch and include: user-visible effect, security/recovery impact, tests run, UE fixture/cleanup details, and paired documentation changes. Public PR CI is intentionally limited to GitHub-hosted checks. A maintainer runs protected UE CI only after the code reaches a trusted ref.

Commit messages use `type(scope): 中文描述` and stay within 72 characters. Valid types are `feat`, `fix`, `refactor`, `perf`, `style`, `docs`, `test`, and `chore`.

A PR may merge before all v1 capabilities exist, but it must not weaken the [v1 release gate](Docs/v1-release-gate.md) or mark an unmet item complete.

## 简体中文

感谢参与 `pi-unreal-blueprint`。项目当前处于 v1 之前，`PLAN.md` 是产品范围和完成标准的权威依据。

### 修改前

1. 阅读 `AGENTS.md`、`PLAN.md`、[产品范围](Docs/product-scope.zh-CN.md) 和 [架构](Docs/architecture.zh-CN.md)。
2. v1 只覆盖 UE4.27 Win64 和 Blueprint 体系。无关领域单独提案，不得暗中扩范围。
3. 修改公共协议、Operation Registry 契约、写入/恢复不变量或发布信任边界前，先开 Issue 讨论。
4. 不得把计划能力写成已经实现。面向用户的行为变化必须同步更新中英文文档。

### 工程要求

- Pi Extension、CLI、Editor 和 Commandlet 共用一套 UE Core 和 Operation Registry。
- 未知操作/字段、类型不匹配、目标歧义和失败必须返回明确稳定错误；禁止模拟成功和静默降级。
- 备份、事务、编译、保存、重载、验证、Journal 及对应恢复测试未完成前，写能力不算完成。
- 公开 Fork 代码只能在 GitHub-hosted Runner 运行。任何让 `pull_request` 触达 self-hosted 基础设施的修改都不会接受。
- 禁止提交凭据、会话 token、本机项目路径/配置、私有项目 Unreal 资产、生成的插件二进制、备份或 Job Journal。

### Pull Request

分支和 PR 保持单一主题，并写清：用户可见变化、安全/恢复影响、已运行测试、UE Fixture 与清理方式、中英文文档变更。公开 PR CI 只做 GitHub-hosted 检查；代码进入可信 ref 后，维护者才运行受保护 UE CI。

提交消息格式为 `type(scope): 中文描述`，不超过 72 字。type 可用 `feat`、`fix`、`refactor`、`perf`、`style`、`docs`、`test`、`chore`。

v1 能力未全部完成时 PR 仍可合并，但不得降低 [v1 发布门槛](Docs/v1-release-gate.zh-CN.md)，也不能提前勾选未满足项。
