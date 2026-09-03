# codex-unreal-blueprint 产品与架构计划

本文是产品范围、架构、协议、安全、测试和 v1.0.0 完成标准的唯一权威依据。

## 产品目标

`codex-unreal-blueprint` 为本机 Codex 提供 UE4.27 Win64 Blueprint 自动化：

```text
Codex
  ├─ unreal-blueprint Skill
  └─ local stdio MCP server
          └─ localhost TCP JSON-RPC 2.0
                  └─ CodexUnrealBlueprint UE4.27 Editor plugin
```

仓库根目录是 Codex plugin 根。`.codex-plugin/plugin.json` 组合 `skills/` 与 `.mcp.json`；MCP bundle 是 Codex 到 UE Transport 的唯一公开入口。项目不提供独立 CLI、npm 发布、远程 HTTP/OAuth、MCP App、自定义 Commandlet、旧宿主兼容层或旧状态迁移。

## v1 范围

- 平台：Windows、本机 Codex、UE4.27 Editor、Win64。
- 资产：完整 Blueprint 体系，包括 Blueprint、Actor/Component、Graph、变量/函数/宏、UMG、AnimBlueprint、Struct、Enum、Interface 和 Blueprint Library。
- 不包含：UE5、Material、Niagara、Sequencer、关卡 Actor 自动化、Runtime 插件和自动源控恢复。
- 测试 Fixture 根目录固定为 `/Game/CodexAutomation/<runId>`。
- 本地状态固定为 `%LOCALAPPDATA%/CodexUnrealBlueprint/`；环境变量统一使用 `CODEX_UNREAL_*`。

## 公共 MCP 契约

仅公开九个工具：`unreal_status`、`unreal_doctor`、`unreal_search`、`blueprint_capabilities`、`blueprint_inspect`、`blueprint_validate`、`blueprint_apply`、`blueprint_job`、`blueprint_verify`。

Operation Registry 是 operation 参数 Schema 的唯一事实来源。MCP 只校验固定 envelope 和必填 `operation` discriminator；未知 envelope 字段必须失败，具体 operation 的未知字段、类型及语义由 UE Registry 严格拒绝。所有结果同时返回可读文本和 `structuredContent`。

读工具标记 `readOnlyHint: true`。`blueprint_apply` 标记 `destructiveHint: true`、`openWorldHint: false`；`blueprint_job` 因支持取消保守标记为非只读。项目不覆盖 Codex 宿主 approval policy。

## 写入安全与故障语义

- 写请求必须携带唯一 `requestId`。响应不明时查询原请求，禁止重放。
- 写入默认自动执行，不增加插件确认框。
- 自动写入必须经过严格预检、UE transaction、operation journal、编译、保存、重载和结构验证。
- 部分失败必须返回 `modified`、`saved`、`notSaved`、`unknown` 资产清单，以及稳定错误码、资产路径、operation 索引、UE callsite 和原始编译信息。
- 不复制 Package、不假装成功、不吞异常、不自动恢复；用户依据精确清单通过 Git/SVN 手工还原。
- 多个 Editor 匹配时必须显式提供 `editorSessionId`，不得选择第一个候选。

## 模块边界

- `src/mcp/`：stdio server、固定工具 Schema、annotations 和错误映射。
- `src/client/`：会话发现、TCP framing、JSON-RPC、超时、取消和不确定写恢复。
- `src/shared/`：协议、JSON、稳定错误及工具到 RPC method 映射。
- `unreal/CodexUnrealBlueprint/Source/CodexUnrealBlueprintCore`：Operation Registry 与全部编辑逻辑。
- `CodexUnrealBlueprintTransport`：localhost 会话和认证。
- `CodexUnrealBlueprintEditor`：状态栏图标与 Tooltip。
- `CodexUnrealBlueprintTests`：UE Automation tests。

MCP 和 Editor 只能调用同一 Core/Registry，禁止建立第二套编辑逻辑或 Schema。

## 安装与发布

`scripts/setup.ps1` 必须真实执行 Node、Codex CLI、VS C++、UE4.27 和 `.uproject` 探测，支持 `-CodexExecutable`，构建单文件 MCP bundle，并以受管 manifest 安全同步个人 Marketplace plugin 和 UE plugin。仅删除上次 manifest 记录的文件；Editor 正在加载插件时在覆盖前失败。

GitHub Release 包含 `CodexUnrealBlueprint-<version>-UE4.27-Win64.zip`、`codex-unreal-blueprint-<version>-codex-plugin.zip`、`SHA256SUMS.txt` 和双语 Release Notes。npm 只用于开发依赖、测试和构建，不发布 package。

## v1.0.0 完成标准

- TypeScript typecheck、Vitest、MCP bundle、manifest validator、Skill validator 全部通过。
- 真实 stdio client/server round trip 验证九个工具、Schema、annotations 和 `structuredContent`。
- 假 Transport 覆盖会话歧义、精确选择、超时、取消、`requestId` 恢复和部分失败清单。
- UE4.27 Win64 插件构建与 `E:/Master/LuaSocial.uproject` Automation tests 通过；写能力验证预检、事务、编译、保存、重载和结构断言。
- setup 在隔离目录覆盖首次安装、重复更新、Marketplace 条目保留、损坏 Codex shim、Editor 占用和受管文件边界。
- Release dry-run 可从空目录安装；`package.json`、`plugin.json`、`.uplugin` 和协议版本一致。
- 当前发布物中不得残留旧产品标识、旧环境变量、旧集成依赖或自定义 Commandlet。
