# Changelog / 更新日志

All notable changes are documented here. This project follows [Semantic Versioning](https://semver.org/).

所有重要变更都记录在这里。本项目遵循 [Semantic Versioning](https://semver.org/lang/zh-CN/)。

## [Unreleased]

Migrated the product to a Codex plugin with a bundled stdio MCP server and fully renamed UE plugin. / 已迁移为包含 stdio MCP server 的 Codex plugin，并完整重命名 UE plugin。

- Added layered `generic`, `specialized`, and `editable` asset capabilities, with Editor-backed Material, Niagara, and AnimMontage inspection plus asset comparison and referencer search. / 新增 `generic`、`specialized`、`editable` 分层资产能力，以及 Editor 内 Material、Niagara、AnimMontage 检查、资产比较和引用查找。
- Bundled the UAssetAPI offline inspector and added `auto`/`editor`/`offline` modes, removing the need for a separate `inspect-unreal-uassets` skill. / 内置 UAssetAPI 离线解析器并增加 `auto`/`editor`/`offline` 模式，不再需要额外安装 `inspect-unreal-uassets` skill。
- Added rolling offline staging for requested packages and companion files. The retained primary-asset count is bounded, companion files do not consume slots, and the oldest snapshots are evicted automatically so locked or offline-only assets can keep being parsed without restarting the Editor. / 新增滚动离线暂存：限制缓存中保留的主资产数量，companion 文件不占名额，满额后自动淘汰最旧快照，让仅离线可解析或被占用资产无需频繁重启 Editor 也能持续解析。
- Removed the versioned `PLAN.md`; architecture, MCP contracts, and release gates now own their respective requirements. / 删除版本化 `PLAN.md`，由架构、MCP 契约和发布门槛文档分别维护对应要求。
- Removed duplicate planning/release documents, stale Pi-era internal identifiers, and unused development-script branches; the remaining build-and-test helper is now `check.ps1`. / 删除重复的规划与发布文档、遗留 Pi 内部标识和未使用的开发脚本分支；保留的构建测试脚本更名为 `check.ps1`。
- Fixed graceful TCP disconnect handling that could leave one busy-loop UE thread per closed connection; added connection, authentication, idle, and send limits plus repeated-disconnect regression coverage. / 修复 TCP 正常断开后每个连接可能遗留一个 UE 空转线程的问题，并补充连接数、认证、空闲和发送期限以及重复断连回归测试。
- Removed discovery-only TCP probes, propagated cancellation through discovery and authentication, and made request timeout or cancellation close the transport deterministically. / 删除仅用于发现会话的 TCP 探测，将取消信号贯穿会话发现与认证，并在请求超时或取消时确定性关闭传输连接。
- Installed the exact validated UE binaries and added per-install Codex cachebusters so updates do not reuse or overwrite an active plugin cache. / 安装实际验证过的 UE DLL，并为每次 Codex 安装生成 cachebuster，避免复用或原地覆盖正在使用的插件缓存。

## [1.0.0]

### Setup and development / 安装与开发

- Added project-level installation as the default at `<Project>/Plugins/CodexUnrealBlueprint`.
- Added optional engine-level installation at `<EngineRoot>/Engine/Plugins/Developer/CodexUnrealBlueprint`.
- Added prerequisite, managed-file hash, and installation doctor checks plus the guarded `check.ps1` build-and-test flow.
- 默认支持安装到 `<Project>/Plugins/CodexUnrealBlueprint` 的项目级安装。
- 可选支持安装到 `<EngineRoot>/Engine/Plugins/Developer/CodexUnrealBlueprint` 的引擎级安装。
- 增加前置环境、受管文件哈希和安装 doctor 检查，以及受保护的 `check.ps1` 构建与测试流程。

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
