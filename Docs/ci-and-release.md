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

## Workflow behavior

Public PR CI checks documentation links and runs TypeScript/package checks on `ubuntu-latest`. Dependency lifecycle scripts are disabled during `npm ci`, checkout credentials are not persisted, permissions are read-only, and no self-hosted runner, Environment, project path, or secret is available.

Protected UE CI checks out only the canonical repository's trusted `main` or `v*` ref on a labeled Windows runner, then runs package checks, `RunUAT BuildPlugin`, and UE Automation through `scripts/dev.ps1 check -RunUnrealTests`. Release additionally checks npm lockfile, UE plugin, C++ plugin constant, protocol constants, tag/input, and changelog version alignment; builds the Launcher UE4.27 Win64 plugin zip; packs npm; generates `SHA256SUMS.txt`; and uses the matching changelog section as GitHub Release notes.

Expected runner configuration is local and secret-free where possible:

- `UE_4_27_ROOT` points to a trusted UE4.27 installation;
- `PI_UNREAL_UPROJECT` points to an isolated/configured test project (the project default is `E:/Master/LuaSocial.uproject`);
- the service account has no broader network or repository write permission than required;
- workspaces and `/Game/PiAutomation/<runId>` fixtures are cleaned by trusted test code, not fork code.

## Release credentials and flow

`NPM_TOKEN` and GitHub release write permission belong only to the `release` Environment. Maintainer approval is required. Pull requests and UE CI never receive publishing credentials. Release jobs use least-privilege workflow permissions and do not run arbitrary PR refs.

A manual dispatch from `main` is verification-only and must keep `dry_run=true`: it performs the same checks/build/package/checksum flow and uploads evidence without publishing. Only a protected `v*` tag can publish, and checkout is pinned to that tag ref; workflows never create or move tags. Release verification rejects legacy RPC surfaces, missing required files, inconsistent versions, and any open v1 gate item for a `1.x` release. Release assets are the UE4.27 Win64 plugin zip, npm `.tgz`, and `SHA256SUMS.txt`. GitHub automatically provides source archives, and npm publication uses provenance. If any gate fails, release stops before publication.

## Required branch settings

1. Protect `main`; require Public PR CI and review before merge.
2. Disallow direct unreviewed pushes and force pushes.
3. Protect `v*` tags.
4. Require reviewers on `ue-ci` and `release` Environments.
5. Restrict both Environments to `main`/protected `v*` tags.
6. Register the UE runner only at the private organization/repository scope and assign all required labels.
