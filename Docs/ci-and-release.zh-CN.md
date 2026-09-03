# CI 与发布模型

[English](ci-and-release.md)

## 信任边界

| Workflow | 触发方式 | Runner | 未信任 Fork 代码 |
|---|---|---|---|
| 公开 PR CI | `pull_request` | GitHub-hosted Ubuntu | 只允许在这里运行，且无 secrets |
| 受保护 UE CI | 可信 `main`、tag、获批人工触发 | 带标签的 self-hosted Windows | 绝不运行 |
| Release | 版本 tag 或获批人工触发 | 带标签的 self-hosted Windows | 绝不运行 |

公开 PR Workflow 只有仓库只读权限，不读取 secrets，不绑定 Environment，也不使用 `self-hosted` 标签。package 实现后可以增加 TypeScript 检查，但仍必须留在 GitHub-hosted Runner。

受保护 Job 同时校验官方仓库、可信 ref、专用标签（`trusted`、`ue4.27`）和受保护 GitHub Environment。仓库管理员必须给 `ue-ci`、`release` 配置审核人，并只允许 `main` 和版本 tag 部署。两个 Workflow 都没有 Fork PR 触发器，Job 条件也会再次拒绝。

## Workflow 行为

公开 PR CI 在 `ubuntu-latest` 检查文档链接并运行 TypeScript/package 检查。`npm ci` 禁用依赖生命周期脚本，checkout 不保留凭据，权限只读，并且拿不到 self-hosted Runner、Environment、项目路径或 secret。

受保护 UE CI 只在带专用标签的 Windows Runner checkout 官方仓库可信 `main` 或 `v*` ref，再通过 `scripts/dev.ps1 check -RunUnrealTests` 运行 package 检查、`RunUAT BuildPlugin` 和 UE Automation。Release 还会校验 npm lockfile、UE 插件、C++ 插件常量、协议常量、tag/input 和 CHANGELOG 版本一致性；构建 Launcher UE4.27 Win64 插件 zip；打 npm 包；生成 `SHA256SUMS.txt`；并用对应 CHANGELOG 段落作为 GitHub Release notes。

Runner 预期配置：

- `UE_4_27_ROOT` 指向可信 UE4.27 安装；
- `PI_UNREAL_UPROJECT` 指向隔离/已配置的测试项目（项目默认值为 `E:/Master/LuaSocial.uproject`）；
- 服务账号只拥有必要的网络和仓库权限；
- Workspace 和 `/Game/PiAutomation/<runId>` Fixture 由可信测试代码清理，绝不让 Fork 代码处理。

## 发布凭据与流程

`NPM_TOKEN` 和 GitHub Release 写权限只属于 `release` Environment，并要求维护者批准。Pull Request 和 UE CI 都拿不到发布凭据。Release Job 使用最小权限，且不会执行任意 PR ref。

从 `main` 人工触发只用于验证，必须保持 `dry_run=true`：执行相同检查、构建、打包和 checksum 流程，只上传证据，不发布。只有受保护的 `v*` tag 能发布，checkout 固定到该 tag ref；Workflow 不创建或移动 tag。Release 校验会拒绝旧 RPC、缺失的必需文件、版本不一致，以及 `1.x` 发布时仍未完成的 v1 gate。Release 资产包括 UE4.27 Win64 插件 zip、npm `.tgz` 和 `SHA256SUMS.txt`，源码压缩包由 GitHub 自动提供，npm 发布启用 provenance。任一门禁失败都会在发布前停止。

## 必须配置的仓库规则

1. 保护 `main`，合并前要求公开 PR CI 和代码评审。
2. 禁止未经评审直接 push，禁止 force push。
3. 保护 `v*` tag。
4. 给 `ue-ci` 和 `release` Environment 配置审核人。
5. 两个 Environment 只允许 `main` / 受保护 `v*` tag。
6. UE Runner 只注册在私有组织/仓库范围，并配置全部专用标签。
