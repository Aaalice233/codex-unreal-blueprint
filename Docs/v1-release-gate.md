# v1.0.0 release gate

[简体中文](v1-release-gate.zh-CN.md)

`v1.0.0` must not be published until every item below has reproducible evidence. Checked boxes belong in an actual release change; this baseline intentionally leaves all items open.

- [ ] Every asset family and operation in the [product scope](product-scope.md) has real UE4.27 C++ and E2E coverage.
- [ ] Every write proves preflight, transaction, mutation, compile, save, reload, structural verification, and a precise affected-package Journal; no package backup or automatic restore is claimed.
- [ ] Failure injection covers compile/save/disk/source-control failure, dirty packages, disconnect timing, crash, and partial save without partial-success responses, and verifies accurate Git/SVN manual-recovery guidance.
- [ ] Reusing a `requestId` after a lost response returns the original result and cannot duplicate work.
- [ ] Multi-read/single-write leases, heartbeat expiry, safe cancellation, and unsafe-stage waiting are tested.
- [ ] Interactive Editor and headless Commandlet produce equivalent structures for identical requests.
- [ ] Persistence tests close/restart and independently inspect saved fixture assets.
- [ ] Pi tools, status UI, CLI/JSON output, setup, doctor, local development flow, and protected CI are implemented and tested.
- [ ] A fresh user can install from the release, build/connect UE4.27, and complete one sandbox modification through one setup flow.
- [ ] npm, UE plugin, protocol, tag, changelog, checksums, and release metadata use one consistent version.
- [ ] English and Chinese setup, Tool/CLI reference, source control, manual recovery, contribution, and security documentation match implemented behavior.
- [ ] No TODO handler, placeholder interface, mock success, silent fallback, duplicated edit core, or undocumented behavior remains in release scope.
- [ ] Maintainer reviews final diff and approves the protected `release` Environment deployment.

A passing documentation workflow is not release evidence. A feature is available only when its implementation and required test layers are present in the tagged commit.
