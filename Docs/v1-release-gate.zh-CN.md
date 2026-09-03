# v1.0.0 发布门槛

- [ ] `npm run check`、Codex plugin validator 和 Skill validator 通过。
- [ ] stdio round trip 枚举十二个 MCP tools，并验证 Schema、annotations、文本与 `structuredContent`。
- [ ] Editor 模式用 Fixture 验证通用资产、Blueprint/UMG/AnimBlueprint、AnimMontage、Material/Material Instance 和 Niagara System 的分层检查。
- [ ] 随包离线检查、比较和有界引用查找在没有 Editor、没有额外 skill 时正常工作。
- [ ] 会话歧义、超时、取消、`requestId` 恢复和部分失败清单有自动测试。
- [ ] UE4.27 Win64 plugin 构建与 Automation tests 通过。
- [ ] setup 的首次/重复安装、Marketplace 保留、损坏 CLI、Editor 占用和受管边界通过。
- [ ] Release 两个 zip 可从空目录安装，四处版本一致，SHA-256 可验证。
- [ ] 发布表面没有旧产品标识、旧依赖、旧环境变量、独立 CLI 或自定义 Commandlet。

English: [v1-release-gate.md](v1-release-gate.md)
