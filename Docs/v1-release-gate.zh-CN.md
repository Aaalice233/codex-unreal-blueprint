# v1.0.0 发布门槛

[English](v1-release-gate.md)

以下每项都必须有可复现证据，才能发布 `v1.0.0`。勾选状态只能随真实发布改动更新；当前基线故意全部留空。

- [ ] [产品范围](product-scope.zh-CN.md) 中每种资产和操作都有真实 UE4.27 C++ 测试与 E2E。
- [ ] 每种写入都证明经过预检、事务、修改、编译、保存、重载、结构验证和准确的受影响 Package Journal；不声称提供 Package 备份或自动恢复。
- [ ] 故障注入覆盖编译/保存/磁盘/Source Control 失败、Dirty Package、不同断线时点、崩溃和部分保存，且不会返回“部分成功”，并验证准确的 Git/SVN 手工还原指引。
- [ ] 响应丢失后复用同一 `requestId` 只返回原结果，不会重复修改。
- [ ] 多读单写租约、心跳过期、安全取消和不可取消阶段等待已有测试。
- [ ] 交互 Editor 与 Headless Commandlet 对同一请求得到等价结构。
- [ ] 持久化测试会关闭/重启，并独立检查保存后的 Fixture 资产。
- [ ] Pi tools、状态界面、CLI/JSON 输出、setup、doctor、本机开发流程和受保护 CI 均已实现并测试。
- [ ] 新用户可从 Release 安装，在一条 setup 流程内构建/连接 UE4.27 并完成一次沙盒修改。
- [ ] npm、UE 插件、协议、tag、CHANGELOG、校验和与 Release 元数据版本一致。
- [ ] 中英文 setup、Tool/CLI、Source Control、手工还原、贡献和安全文档与真实实现一致。
- [ ] 发布范围内不存在 TODO handler、占位接口、模拟成功、静默降级、重复编辑核心或未说明行为。
- [ ] 维护者完成最终 diff 评审，并批准受保护 `release` Environment 部署。

文档检查通过不等于达到发布标准。只有实现和对应测试都存在于 tag 指向的提交中，功能才算可用。
