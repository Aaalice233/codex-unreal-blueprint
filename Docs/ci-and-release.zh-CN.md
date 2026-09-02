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

## 当前骨架行为

实现或产物输入缺失时，受保护 Workflow 的就绪检查会明确失败，不会用占位步骤制造成功。实现落地后，UE Job 将调用仓库内已提交的校验入口，在 Runner 配置的 UE4.27 项目上执行。Release 随后校验版本一致性、运行完整测试、用 `RunUAT BuildPlugin` 构建插件、打 npm 包、生成 SHA-256，并且只通过获批 `release` Environment 发布。

Runner 预期配置：

- `UE_4_27_ROOT` 指向可信 UE4.27 安装；
- `PI_UNREAL_UPROJECT` 指向隔离/已配置的测试项目（项目默认值为 `E:/Master/LuaSocial.uproject`）；
- 服务账号只拥有必要的网络和仓库权限；
- Workspace 和 `/Game/PiAutomation/<runId>` Fixture 由可信测试代码清理，绝不让 Fork 代码处理。

## 发布凭据

`NPM_TOKEN` 和 GitHub Release 写权限只属于 `release` Environment，并要求维护者批准。Pull Request 和 UE CI 都拿不到发布凭据。Release Job 使用最小权限，且不会执行任意 PR ref。

## 必须配置的仓库规则

1. 保护 `main`，合并前要求公开 PR CI 和代码评审。
2. 禁止未经评审直接 push，禁止 force push。
3. 保护 `v*` tag。
4. 给 `ue-ci` 和 `release` Environment 配置审核人。
5. 两个 Environment 只允许 `main` / 受保护 `v*` tag。
6. UE Runner 只注册在私有组织/仓库范围，并配置全部专用标签。
