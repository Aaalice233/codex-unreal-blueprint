# Security policy / 安全策略

## English

### Supported versions

No supported release exists yet. The repository is pre-v1 and must not be used to modify production assets. This file will list supported release lines after v1.0.0 is published.

### Reporting a vulnerability

Do not open a public issue for a suspected vulnerability. Use GitHub's **Security → Report a vulnerability** private reporting flow for this repository. Include the affected commit/version, threat model, reproduction, impact, and any suggested mitigation. Do not include private Unreal project assets, credentials, or live session tokens.

Maintainers will acknowledge a complete report within 7 days, triage it privately, and coordinate disclosure after a fix is available. These are response targets, not a bug-bounty promise.

### Security boundaries

High-impact areas include localhost authentication/session descriptor permissions, stdio and JSON-RPC framing limits, operation/type validation, action catalog integrity, `requestId` idempotency, write leases, package-state reporting and manual recovery, path and mount policy, Source Control handling, log redaction, setup/plugin installation, and CI/release trust boundaries.

The service must bind only to `127.0.0.1`, use a random per-session token, and keep session files current-user-only. This reduces exposure but does not make untrusted local code safe. Never expose the port, token, request files, or Journal through network shares or public CI artifacts. The product stores no asset backup and never automatically runs Git/SVN restore commands.

Public Fork pull requests must never execute on self-hosted runners or receive secrets. Protected UE and release jobs accept only the canonical repository's trusted `main`/protected version tags or approved manual dispatches. GitHub Environment review and branch/tag protection are mandatory operational controls; workflow YAML alone is not sufficient.

## 简体中文

### 支持版本

目前没有受支持的正式版本。仓库处于 v1 之前，不应拿来修改生产资产。发布 v1.0.0 后，本文才会列出受支持的版本线。

### 报告漏洞

疑似漏洞不要开公开 Issue。请使用本仓库 GitHub **Security → Report a vulnerability** 私密报告入口，并提供受影响提交/版本、威胁模型、复现步骤、影响和建议缓解方案。不要上传私有 Unreal 项目资产、凭据或仍有效的会话 token。

维护者目标是在收到完整报告后 7 天内确认，随后私下分级，并在修复可用后协调披露。该时间是响应目标，不代表漏洞赏金承诺。

### 安全边界

高风险区域包括：本机认证和会话文件权限、stdio 与 JSON-RPC 帧限制、操作/类型校验、Action Catalog 完整性、`requestId` 幂等、写租约、Package 状态报告与手工还原、路径和挂载策略、Source Control、日志脱敏、setup/plugin 安装，以及 CI/发布信任边界。

服务必须只监听 `127.0.0.1`，每次会话生成随机 token，会话文件只允许当前用户读取。这只能缩小暴露面，不能让不可信本机代码变安全。禁止通过网络共享或公开 CI Artifact 暴露端口、token、请求文件或 Journal。产品不保存资产备份，也不自动执行 Git/SVN 还原命令。

公开 Fork Pull Request 绝不能在 self-hosted Runner 上运行，也不能得到 secrets。受保护 UE/Release Job 只接受官方仓库可信 `main`、受保护版本 tag 或获批人工触发。GitHub Environment 审批和 branch/tag 保护是必须由管理员配置的控制；只有 Workflow YAML 不够。
