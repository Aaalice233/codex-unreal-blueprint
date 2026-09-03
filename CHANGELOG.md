# Changelog / 更新日志

All notable changes are documented here. This project follows [Semantic Versioning](https://semver.org/).

所有重要变更都记录在这里。本项目遵循 [Semantic Versioning](https://semver.org/lang/zh-CN/)。

## [Unreleased]

Migrated the product to a Codex plugin with a bundled stdio MCP server and fully renamed UE plugin. / 已迁移为包含 stdio MCP server 的 Codex plugin，并完整重命名 UE plugin。

## [1.0.0]

### Setup and development / 安装与开发

- Added project-level installation as the default at `<Project>/Plugins/CodexUnrealBlueprint`.
- Added optional engine-level installation at `<EngineRoot>/Engine/Plugins/Developer/CodexUnrealBlueprint`.
- Added prerequisite, managed-file hash, and installation doctor checks plus the guarded `dev.ps1` check/sync/publish flow.
- 默认支持安装到 `<Project>/Plugins/CodexUnrealBlueprint` 的项目级安装。
- 可选支持安装到 `<EngineRoot>/Engine/Plugins/Developer/CodexUnrealBlueprint` 的引擎级安装。
- 增加前置环境、受管文件哈希和安装 doctor 检查，以及受保护的 `dev.ps1` check/sync/publish 流程。

### Safety and documentation / 安全与文档

- Documented automatic Blueprint writes with strict preflight, transaction, compile, save, reload, verification, and precise partial-failure reporting.
- Clarified that the product creates no package backup and performs no automatic Git/SVN restore; recovery is a user-reviewed manual source-control action.
- Added English and Simplified Chinese fresh-install, doctor, local development, CI/release, and manual recovery guidance.
- 记录 Blueprint 自动写入所需的严格预检、事务、编译、保存、重载、验证和准确部分失败报告。
- 明确产品不创建 Package 备份，也不自动执行 Git/SVN 还原；资产还原必须由用户核对后手工完成。
- 补充中英文从零安装、doctor、本机开发、CI/Release 和手工还原说明。

### CI and release / CI 与发布

- Isolated public Fork PR checks on GitHub-hosted runners with read-only permissions and no secrets.
- Added protected self-hosted UE4.27 compile/E2E checks for trusted refs only.
- Added the approved `v1.0.0` release flow for UE4.27 Win64 and Codex plugin zip assets, `SHA256SUMS.txt`, and changelog-based release notes, with cross-file version validation.
- 公开 Fork PR 只在 GitHub-hosted Runner 使用只读权限运行，不接触 secret。
- 受保护 self-hosted UE4.27 编译/E2E 只运行可信 ref。
- 增加经审批的 `v1.0.0` Release 流程，生成 UE4.27 Win64 与 Codex plugin zip、`SHA256SUMS.txt` 和 CHANGELOG Release notes，并校验跨文件版本一致性。
