# CI and release model

[简体中文](ci-and-release.zh-CN.md)

## Trust boundary

| Workflow | Trigger | Runner | Untrusted fork code |
|---|---|---|---|
| Public PR CI | `pull_request` | GitHub-hosted Ubuntu | Allowed only here; no secrets |
| Protected UE CI | trusted `main`, tags, approved dispatch | labeled self-hosted Windows | Never |
| Release | version tag or approved dispatch | labeled self-hosted Windows | Never |

The public PR workflow has read-only repository permission, no secrets, no environment, and no `self-hosted` label. It may add TypeScript checks after the package exists, but all such checks must remain on GitHub-hosted runners.

Protected jobs require the canonical repository, a trusted ref, dedicated labels (`trusted`, `ue4.27`), and protected GitHub Environments. Repository administrators must configure `ue-ci` and `release` with required reviewers and restrict deployment branches to `main` and version tags. Fork PR events are absent from both workflow triggers and rejected by job conditions.

## Current skeleton behavior

The protected workflows intentionally fail their readiness check while implementation/artifact inputs are absent. They do not report placeholder success. Once implementation lands, the UE job will invoke the repository's checked-in validation entry point against the runner's configured UE4.27 project. Release then checks version alignment, runs complete tests, builds the plugin with `RunUAT BuildPlugin`, packs npm, emits SHA-256 files, and publishes only through the approved `release` environment.

Expected runner configuration is local and secret-free where possible:

- `UE_4_27_ROOT` points to a trusted UE4.27 installation;
- `PI_UNREAL_UPROJECT` points to an isolated/configured test project (the project default is `E:/Master/LuaSocial.uproject`);
- the service account has no broader network or repository write permission than required;
- workspaces and `/Game/PiAutomation/<runId>` fixtures are cleaned by trusted test code, not fork code.

## Release credentials

`NPM_TOKEN` and GitHub release write permission belong only to the `release` Environment. Maintainer approval is required. Pull requests and UE CI never receive publishing credentials. Release jobs use least-privilege workflow permissions and do not run arbitrary PR refs.

## Required branch settings

1. Protect `main`; require Public PR CI and review before merge.
2. Disallow direct unreviewed pushes and force pushes.
3. Protect `v*` tags.
4. Require reviewers on `ue-ci` and `release` Environments.
5. Restrict both Environments to `main`/protected `v*` tags.
6. Register the UE runner only at the private organization/repository scope and assign all required labels.
