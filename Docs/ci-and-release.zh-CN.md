# CI 与发布

普通 CI 运行 TypeScript typecheck、Vitest、MCP bundle、Codex plugin validator、Skill validator 和残留标识扫描。受保护 Windows runner 使用 UE4.27 构建 Win64 plugin，并可对 `E:/Master/LuaSocial.uproject` 运行 Automation tests。

Release dry-run 校验四处版本一致并生成：

- `CodexUnrealBlueprint-<version>-UE4.27-Win64.zip`
- `codex-unreal-blueprint-<version>-codex-plugin.zip`
- `SHA256SUMS.txt`
- 双语 Release Notes

项目不执行 npm publish 或 npm provenance。两个 zip 必须可从空目录安装。

English: [ci-and-release.md](ci-and-release.md)
