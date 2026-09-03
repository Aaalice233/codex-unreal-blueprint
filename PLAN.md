# `pi-unreal-blueprint` 生产级完整成品实施计划

## 目标与已定方向

创建公开 MIT 项目 `Aaalice233/pi-unreal-blueprint`，本地克隆到 `E:/pi-unreal-blueprint`。第一版即按 `v1.0.0` 完整成品标准交付，不发布功能残缺的正式版本。

产品边界是 **UE4.27 Win64 的完整 Blueprint 体系自动化**，不是整个 Unreal Editor 自动化：覆盖普通 Blueprint、组件、变量、函数、宏、事件、Event Dispatcher、Interface、Function/Macro Library、Level Blueprint、UMG、AnimBlueprint、UserDefinedStruct 和 UserDefinedEnum；不把 Material、Niagara、Sequencer、关卡摆放等非 Blueprint 领域混入第一版。

核心链路：

```text
Pi Extension / CLI
        │
        ├─ 本机 TCP JSON-RPC 2.0
        │
PiUnrealBlueprint UE Editor Plugin
        │
        └─ UE4.27 原生 Editor C++ API
```

- 不使用 MCP 作为第一版入口；稳定 CLI 和 JSON-RPC 已为未来 MCP 薄适配保留边界。
- 不依赖、不链接、不分发 AQ；AQ 仅作为本机能力对照，所有发布能力由自研 UE 插件独立实现。
- Pi 写入完全自动执行，不弹计划确认框；安全性由严格前置校验、UE 事务、编译保存回读、幂等审计和准确的部分失败清单保证。资产通过项目现有 Git/SVN 手工还原，不实现文件备份或自动恢复。

## 仓库与发布形态

仓库采用 Monorepo：

```text
pi-unreal-blueprint/
├─ AGENTS.md
├─ LICENSE
├─ README.md / README.zh-CN.md
├─ package.json
├─ extensions/                 # Pi 原生 Extension、Tool、状态栏和命令面板
├─ skills/unreal-blueprint/    # Agent Skill，仅描述调用流程和约束
├─ src/
│  ├─ client/                  # JSON-RPC 客户端、会话发现、Job/进度
│  ├─ cli/                     # 稳定 CLI
│  └─ shared/                  # 公共 envelope、错误码、日志类型
├─ unreal/PiUnrealBlueprint/   # UE4.27 Editor 插件完整源码
├─ scripts/dev.ps1             # 本机开发、同步、提交、推送统一入口
├─ scripts/setup.ps1           # 可由 CLI setup 调用的安装实现
├─ tests/                      # TS、协议、故障注入和 E2E 驱动
└─ .github/workflows/          # PR CI、受保护 UE CI、Release
```

发布内容：

- npm：非 scoped 包 `pi-unreal-blueprint`，带 `pi-package` keyword，manifest 加载 `extensions` 和 `skills`。
- GitHub Release：源码、UE4.27 Launcher Win64 预编译插件、校验和、变更日志。
- UE 源码始终发布；自定义源码引擎由 `setup` 调用目标引擎工具链编译。
- 默认项目级安装到 `<Project>/Plugins/PiUnrealBlueprint`，可显式选择引擎级 `Engine/Plugins/Developer/PiUnrealBlueprint`。
- 文档默认中英双语；不采集遥测，只保留本机结构化日志。

## 首先更新开发规则

Plan 模式结束、进入实施后，先更新当前被 SVN 忽略的 `E:/Master/Content/Lua/AGENTS.md`，并在新仓库创建 `AGENTS.md`，记录用户已明确选择的偏好：

- 本产品的 Blueprint 写入默认完全自动执行，不增加确认弹窗。
- 自动写入必须具备严格预检、UE 事务、操作日志、编译保存回读和部分失败清单；不复制 Package、不实现自动恢复，资产由用户通过 Git/SVN 手工还原。
- 本机 UE 集成测试使用 `E:/Master/LuaSocial.uproject`；产品开发任务获准自动操作 E:/Master 资产，不再受原“禁止 Agent 操作 UE Editor”条款限制。
- UE 插件只提供状态栏图标，不建设 Dock 管理面板；详细管理集中在 Pi 面板和 CLI。
- 不引入需要长期维护的“版本化 Plan 文件”；批处理只接受当次普通 JSON 请求。

规则修改只针对该产品开发，不放宽其他普通 Lua/蓝图任务的操作权限。

## UE 插件架构

插件仅在 Editor/Commandlet 环境加载，不进入游戏 Runtime 包：

- **Core**：资产解析、操作注册表、预检、事务、编译、保存、回读和失败状态归档；不依赖 Slate，供交互 Editor 与 Commandlet 共用。
- **Transport**：本机 TCP 服务、JSON-RPC、认证、会话发现、Job 队列和进度通知。
- **Editor Integration**：状态栏图标与 Tooltip，显示未连接/已连接/写入中/故障；不提供 Dock 面板。
- **Commandlet**：调用同一 Core，从请求文件读取普通 JSON，输出结构化结果，支持 CI/headless 写入。
- **Tests**：C++ Automation Tests 和故障注入接口，仅在测试构建启用。

所有 UObject、Graph 和 Package 修改切回 Game Thread；网络线程只负责收发和解析。长任务按可安全拆分的阶段让出执行机会，编译等 UE 原生不可中断阶段明确标记。

## 通信、发现与并发

- Editor 仅监听 `127.0.0.1` 的随机端口，使用长度前缀帧，限制单帧大小、请求深度和批量数量。
- 每次 Editor 启动生成 `editorSessionId`、随机令牌和协议版本；会话描述写入 `%LOCALAPPDATA%/PiUnrealBlueprint/sessions/`，文件限定当前用户访问。
- 会话信息包括 PID、`.uproject` 规范路径、引擎版本、端口、插件版本、协议版本和能力摘要。
- Pi 依据 `.uproject` 精确匹配；多开时不使用“第一个 Editor”，歧义由 Pi 面板选择并记住到当前会话。
- 采用多读单写租约：检查可并发；写 Job 持有带心跳和超时的互斥租约。
- 每个写请求携带 `requestId`。Editor 保存请求状态和结果；断线后客户端查询原请求，禁止直接重放写命令。
- 协议采用 SemVer 能力协商；主版本不兼容时拒绝执行并给出 setup/升级指引。

## Blueprint 能力范围

### 资产与类型系统

支持创建、复制、重命名、移动、删除、重设父类、实现/移除 Interface、修复目标 Redirector，以及读取和修改 Class Defaults。

变量和 Pin 使用完整 `FEdGraphPinType`：基础类型、Object/Class、Soft/Weak Reference、Interface、Enum、Struct、Delegate、Array、Set、Map、引用/Const，以及分类、Tooltip、编辑可见性、ExposeOnSpawn、Replication 等 Blueprint 元数据。

### 组件与默认值

支持 SCS 组件增删改查、继承组件覆盖、重命名、Root、挂接关系、Transform、任意可编辑反射属性、资产引用、容器属性和组件模板回读。所有属性写入先根据 `FProperty` 做真实类型校验，拒绝字符串猜类型和静默转换。

### Graph、函数、事件与宏

支持 Event/Construction/Function/Macro/Delegate/Interface/LevelScript Graph：

- 函数、宏、自定义事件、Event Dispatcher、局部变量和完整签名。
- 节点创建、删除、移动、复制、注释、Reroute、默认 Pin 值、连线、断线、重建和刷新。
- 通过 `FBlueprintActionDatabase`/Node Spawner 建立 **Action Catalog**，而不是裸构造任意 `UK2Node`。
- `unreal_search` 返回 actionId、成员签名、上下文兼容性和 Pin 摘要；生成节点只接受当前 Catalog 中的 actionId。
- actionId 由规范化动作描述生成，并绑定 Catalog 指纹；插件或工程变化导致动作消失时明确失败，不回退到相似项。
- 节点、资产、成员和 Pin 都返回稳定路径/GUID；多个候选时返回候选列表，绝不按名称评分静默猜测。
- 提供确定性自动布局；默认只布局本次新增/受影响子图，除非请求明确要求重排整个 Graph。

### UMG

支持 WidgetBlueprint 的 WidgetTree 创建、删除、重命名、层级调整、Named Slot、Panel Slot 属性、Widget 属性、变量暴露、事件/属性绑定、导航与可访问性设置；支持 Widget Animation 的绑定、MovieScene Track、Section 和关键帧编辑。修改后同时验证 WidgetTree、GeneratedClass 和绑定目标。

### AnimBlueprint

支持 Skeleton/Parent 设置、AnimGraph 节点、State Machine、State、Transition、Transition Rule、Conduit、Pose Link、变量和 Event Graph；使用 Animation Blueprint 对应 Schema/Spawner 校验上下文，不把普通 K2 Graph 规则硬套到 AnimGraph。

### Struct、Enum 与专用 Blueprint

支持 UserDefinedStruct 字段和默认值、UserDefinedEnum 枚举项、Blueprint Interface、Function Library、Macro Library、Level Blueprint。Level Blueprint 修改会把所属 Map 纳入影响资产集合和失败状态清单。

## Pi Tool 公共接口

采用少量高层 Tool 和按需发现的强类型 Operation Registry，避免把数百个 Tool 或巨型 Schema 常驻模型上下文：

1. `unreal_status`：发现/选择 Editor，会话、版本、PIE、Source Control、Dirty Package 和队列状态。
2. `unreal_doctor`：检查 Pi 包、UE 插件、协议、编译环境、项目配置、端口和权限。
3. `unreal_search`：分页搜索资产、Class、成员、属性、Graph Action 和可用操作。
4. `blueprint_capabilities`：按领域返回当前插件真实支持的 operation 名、参数 JSON Schema 和示例。
5. `blueprint_inspect`：按 facet 分页读取资产、组件、变量、Graph、节点、Pin、UMG、AnimBP、编译状态和结构哈希。
6. `blueprint_validate`：只在内存解析本次普通操作列表，返回类型、引用、歧义、Source Control 和影响资产错误；不生成持久 Plan。
7. `blueprint_apply`：接收一次强类型 operation 列表，内部再次校验后启动自动写入 Job。
8. `blueprint_job`：查询、等待或在安全阶段取消 Job，并读取结构化进度和编译日志。
9. `blueprint_verify`：独立编译/重载/结构断言，支持检查调用方给出的期望条件。

`blueprint_apply` 的公开 Tool Schema 保持紧凑；具体 operation 参数由 UE Core 的 Operation Registry 作为唯一事实来源，Pi 通过 `blueprint_capabilities` 按需取得。未知字段、未知 operation 和不匹配类型全部拒绝。

## 自动写入、安全与失败处置

每个写 Job 固定执行：

1. 锁定准确 Editor 会话并获取写租约。
2. 解析所有引用和 actionId，计算完整影响 Package 集合。
3. 拒绝目标中已有用户未保存的 Dirty Package，并列出具体资产。
4. 检查 expectedStateHash、磁盘空间、只读状态和 UE Source Control Provider。
5. 按需 Checkout；新文件在成功保存后 MarkForAdd。Provider 失败即停止，不绕过团队流程。
6. 创建 `FScopedTransaction`，对 Blueprint、Graph、Node、SCS、Component Template 等完整调用 `Modify()`。
7. 应用全部 operation，结构回读并校验预期结果。
8. 按依赖顺序统一编译，收集 `FCompilerResultsLog`；Error 必须为 0，Warning 单独返回但默认不阻止保存。
9. 再次校验整批后保存 Package；逐个记录保存结果，不声称磁盘级原子提交。
10. 重新加载/重新查询成功保存的资产，验证磁盘状态、结构哈希、编译状态和 Package 非 Dirty。
11. 写入 requestId/Job 审计、释放租约并返回实际变更摘要。

失败处理：

- 保存前失败：通过 `FScopedTransaction` 撤销并验证内存结构；它只提供 UE Editor 内的 Undo，不承担跨进程或磁盘恢复。
- 部分保存、Editor 崩溃或连接不明：不自动复制、覆盖、删除或还原 Package；Journal 记录 `modified/saved/notSaved/unknown` 资产、operation 索引、执行阶段和最后确认哈希。
- 失败结果必须明确标记 `partial` 或 `stateUnknown`，不得返回成功；同时按 Git/SVN 工作副本类型生成只读检查和手工还原建议。
- 插件不得自动执行 `git reset/checkout`、`svn revert` 或 Source Control 通用 revert，避免覆盖用户后续改动；用户核对清单后自行还原。
- Job Journal 仅用于 requestId 幂等、状态查询、诊断和审计，不保存资产副本，也不提供 history/restore 产品入口。

## 异步 Job 与体验

- `blueprint_apply` 快速返回 `jobId`；Extension 通过通知显示 Preflight、Modify、Compile、Save、Reload、Verify、Failed 阶段及资产级进度。
- 保存前的安全点允许取消并撤销 UE 事务；编译/保存等不可安全打断阶段返回“等待当前阶段结束”，不伪造取消成功。
- Pi Footer 显示 `UE: <Project> • Connected/Job/Failed`。
- `/unreal-blueprint` 面板管理 Editor 会话、Job、错误、部分失败资产清单和 doctor；不增加写入确认步骤。
- UE 端仅状态栏图标和 Tooltip，显示服务、客户端、租约和故障状态；详细诊断在 Pi 面板/CLI 完成。
- 错误采用稳定错误码、资产路径、operation 索引、UE 调用位置、原始编译消息和建议动作，不吞异常、不返回“部分成功”冒充成功。

## CLI、Headless 与安装

稳定 CLI 提供人类输出和 `--json`：

```text
pi-unreal-blueprint setup|doctor|status
pi-unreal-blueprint search|inspect|capabilities
pi-unreal-blueprint validate|apply|job|verify
pi-unreal-blueprint plugin install|update|remove
```

- `setup` 一次完成 `.uproject` 选择、项目级/引擎级安装、版本匹配、编译和 doctor；普通 Tool 调用不会偷偷安装或升级插件。
- Headless 模式由 CLI 启动 `UE4Editor-Cmd.exe <uproject> -run=PiUnrealBlueprint ...`，通过请求/结果文件调用同一 C++ Core，不走另一套编辑实现。
- JSON-RPC 和 CLI 在 v1 内保持兼容；TypeScript Core 不作为公共 SDK 承诺，避免多维护一层 API。

## 本机开发与 `dev.ps1`

本机默认配置写入 gitignored 的 `dev.local.json`：

```json
{
  "repo": "E:/pi-unreal-blueprint",
  "piAgentDir": "C:/Users/Admin/.pi/agent",
  "uproject": "E:/Master/LuaSocial.uproject",
  "uePluginTarget": "E:/Master/Plugins/PiUnrealBlueprint",
  "engineRoot": "E:/UE_4.27"
}
```

首次执行 `./scripts/dev.ps1 setup`：验证 Node/Pi/VS/UE4.27，执行本地路径形式的 `pi install E:/pi-unreal-blueprint`，同步 UE 插件并运行 doctor。

统一脚本动作：

```powershell
./scripts/dev.ps1 check
./scripts/dev.ps1 sync
./scripts/dev.ps1 publish -Message "feat(graph): 添加节点动作目录"
```

- `check`：格式、Lint、类型、TS 测试、C++/协议检查和可配置 UE 测试。
- `sync`：先检查，再构建 Pi Extension/CLI，安全镜像插件到唯一受管目标目录并验证文件清单。
- `publish`：先做全部 preflight；验证提交消息符合 `type(scope): 中文描述`，展示变更摘要，非交互 `git add/commit/push` 当前分支，再同步该提交。
- 不执行 force push、reset、clean、stash，不触碰仓库外的非受管文件；目标插件目录只删除上次 manifest 记录的本插件旧文件。
- 检测到 UE Editor 正在运行且 C++ 插件需要更新时，`sync` 在覆盖前停止；`publish` 的 Git 提交和云端推送不受影响，只延后本机插件同步并明确提示关闭 Editor 后运行 `sync`。不尝试 Live Coding、不自动关闭 Editor、不覆盖已加载 DLL。
- Pi 侧本地安装直接引用 E 盘仓库，脚本只构建；用户执行 `/reload` 或重启 Pi 加载新版本。

## 测试与 CI

本机和自托管 Runner 使用 `E:/Master/LuaSocial.uproject`，但自动测试默认只生成唯一 `/Game/PiAutomation/<runId>` Fixture 并在验证后清理；测试框架允许显式指定 E:/Master 其他资产，以覆盖用户已授权的真实项目场景。测试工程路径配置化，公开用户可替换为自己的 UE4.27 项目。

测试矩阵：

- TS：Tool Schema、RPC framing、会话发现、版本协商、超时、取消、错误映射、CLI JSON 输出、开发脚本 dry-run。
- C++：每类 operation 的成功、参数类型错误、继承限制、Action Catalog 歧义、Graph Schema 不兼容、UMG/AnimBP 专用规则。
- 资产族 E2E：普通 BP、组件 BP、Function/Macro Library、Interface、Level BP、Widget BP 与动画、AnimBP 与状态机、Struct、Enum。
- 持久化 E2E：修改→编译→保存→关闭/重启 Editor 或 Commandlet→重新加载→结构断言。
- 失败注入：编译错误、保存失败、磁盘空间不足、Source Control 拒绝、Dirty Package、断线发生在执行前/执行后、Editor 崩溃和部分保存；逐项验证失败分类、资产状态清单与 Git/SVN 手工恢复指引。
- 幂等：写操作已执行但响应丢失时，同一 requestId 只能返回原结果，不能重复创建节点或组件。
- 并发：多读、写租约竞争、心跳失效和取消。
- 独立验证：关键 Fixture 保存后使用静态资产检查核对磁盘内容，避免只相信插件自身回读。

GitHub Actions：

- 公共 PR 仅在 GitHub 托管 Runner 执行 TS、Schema、文档和静态检查，绝不触达本机。
- 受保护的自托管 Windows Runner 只允许 `main`、tag 和人工 `workflow_dispatch`，运行 UE4.27 编译与完整 E2E；不执行来自未信任 Fork 的代码。
- Release Workflow 校验 npm/插件/协议版本一致，运行 `RunUAT BuildPlugin` 生成 Launcher UE4.27 Win64 包，执行完整 E2E，生成 SHA-256、GitHub Release 和 npm 包；发布凭据放 GitHub Environment 并要求维护者批准。

## 完成标准

只有同时满足以下条件才发布 `v1.0.0`：

- 上述所有 Blueprint 资产族和操作领域都有真实 UE4.27 E2E，不存在 TODO、模拟成功或未接通占位接口。
- 所有写入都经过预检、UE 事务、编译、保存、重载、验证和审计；故障注入证明不会盲目重放或把部分失败伪装成成功，并能准确给出 Git/SVN 手工还原所需资产清单。
- 交互 Editor 与 Headless Commandlet 对同一请求得到等价结构结果。
- Pi Tool、状态面板、CLI、setup/doctor、本机开发同步和受保护 Release 流水线全部可用。
- 新安装用户能从 npm/GitHub 在一条 setup 流程内完成项目插件安装、编译、连接和首个沙盒修改。
- README、中文文档、Tool/CLI 参考、失败处置、Git/SVN 手工还原、Source Control、贡献指南和安全说明完整。

## 明确默认与非目标

- 首版仅 UE4.27 Win64，内部隔离版本适配层，但不声称支持 UE5。
- 默认允许写 `/Game` 和项目插件 Content；Engine/引擎插件/第三方挂载点只读，用户可在项目策略中逐项开启。
- 写入完全自动，不弹确认；Dirty 用户资产仍必须拒绝，以免覆盖未保存工作。
- 不保存长期“Plan 文件”；Job Journal 仅保留 requestId 幂等、状态查询、诊断和审计所需数据，不含资产副本。
- 不提供 MCP、公共 TypeScript SDK、独立桌面 GUI或 UE Dock 面板。
- 不把非 Blueprint 的 Material、Niagara、Sequencer、关卡 Actor 自动化扩入 v1。
