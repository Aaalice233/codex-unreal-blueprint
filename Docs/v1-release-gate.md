# v1.0.0 release gate

- [ ] `npm run check`, Codex plugin validation, and Skill validation pass.
- [ ] A stdio round trip enumerates twelve MCP tools and verifies schemas, annotations, text, and `structuredContent`.
- [ ] Layered asset inspection verifies generic assets, Blueprint/UMG/AnimBlueprint, AnimMontage, Material/Material Instance, and Niagara System fixtures in Editor mode.
- [ ] Bundled offline inspection, comparison, and bounded referencer search work without an Editor or any separately installed skill.
- [ ] Automated tests cover session ambiguity, timeout, cancellation, `requestId` recovery, and partial-failure lists.
- [ ] Protocol `2.0.0` validate/verify jobs, global structure hashes, package roles, component ranges/assertions, source-control semantics, and session heartbeats have automated coverage.
- [ ] The 40-component Niagara clone-range performance E2E reports all six write phases and stays within its configured regression budget.
- [ ] Clone-range validate with inherited Third Blueprints and a ResourceMap-like class referencer completes without loading the reference-only package and reports timing, role, and referencer statistics.
- [ ] The UE4.27 Win64 plugin build and Automation tests pass.
- [ ] Setup covers first/repeat install, Marketplace preservation, broken CLI, Editor occupancy, and managed boundaries.
- [ ] Both release archives install from empty directories, four versions agree, and SHA-256 checks pass.
- [ ] Release surfaces contain no legacy identity, dependency, environment variable, standalone CLI, or custom Commandlet.

`CodexUnrealBlueprint.E2E.Performance.NiagaraCloneRange40` defaults to a 60,000 ms pipeline budget. Set `CODEX_UNREAL_PERF_BUDGET_MS` to apply a deliberate machine- or CI-specific budget; the test log reports wall, pipeline, and reload durations separately.

中文：[v1-release-gate.zh-CN.md](v1-release-gate.zh-CN.md)
