# CI and release

Regular CI runs TypeScript typecheck, Vitest, MCP bundling, the Codex plugin validator, the Skill validator, and legacy-identity scans. A protected Windows runner builds the UE4.27 Win64 plugin and may run Automation tests against `E:/Master/LuaSocial.uproject`.

A release dry run verifies the four version declarations and produces:

- `CodexUnrealBlueprint-<version>-UE4.27-Win64.zip`
- `codex-unreal-blueprint-<version>-codex-plugin.zip`
- `SHA256SUMS.txt`
- bilingual release notes

The project does not run npm publish or npm provenance. Both archives must install from an empty directory.

中文：[ci-and-release.zh-CN.md](ci-and-release.zh-CN.md)
