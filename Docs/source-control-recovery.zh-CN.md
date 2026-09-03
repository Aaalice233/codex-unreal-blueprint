# Source Control 与手工资产还原

[English](source-control-recovery.md)

## 产品边界

`codex-unreal-blueprint` **不会**复制或备份 Unreal Package，也**不会**自动执行 Git/SVN 还原命令。`FScopedTransaction` 只能撤销保存前的 Editor 内存修改；它不是磁盘备份，无法恢复崩溃或部分保存。

发生部分保存、Editor 崩溃或连接状态不明时，Job 结果和本机 Journal 会把每个受影响 Package 标成 `modified`、`saved`、`notSaved` 或 `unknown`，并记录 operation 索引、阶段和最后确认哈希。结果必须是 `partial` 或 `stateUnknown`，不能冒充成功。

## 修改资产前

1. 先提交或记录当前 Git/SVN 工作副本状态。
2. 保存无关 Editor 工作；目标 Package 为 Dirty 时写入会被拒绝。
3. 确认 Unreal Source Control Provider 状态；Checkout 失败会停止 Job。
4. 将无关的本地资产改动分开，避免手工还原时覆盖它们。

## Git：先检查，再手工还原

将示例路径替换为失败 Job 返回的准确 Package 清单。`/Game/UI/WBP_Menu` 这类 Unreal Package 通常对应项目内 `Content/UI/WBP_Menu.uasset`。

```powershell
git status --short -- Content/UI/WBP_Menu.uasset
git diff --stat -- Content/UI/WBP_Menu.uasset
git diff --numstat -- Content/UI/WBP_Menu.uasset
```

确认文件只包含本次 Job 的非预期改动后，再执行：

```powershell
git restore --source=HEAD -- Content/UI/WBP_Menu.uasset
```

新建且未跟踪的资产要先核对，只在确认完全属于失败 Job 时才手工删除。插件不会替你执行 `git restore`、`git checkout`、`git reset`、`git clean` 或删除文件。

## SVN：先检查，再手工还原

```powershell
svn status Content/UI/WBP_Menu.uasset
svn diff --summarize Content/UI/WBP_Menu.uasset
svn info Content/UI/WBP_Menu.uasset
```

确认文件只包含本次 Job 的非预期改动后，再执行：

```powershell
svn revert Content/UI/WBP_Menu.uasset
```

对新增文件，先核对 add 状态，再决定是否 `svn revert` 或删除本地文件。插件不会替你执行 `svn revert`、修改 changelist 或删除文件。

## 手工还原后

1. 重启或重新加载受影响 Package/Editor，使内存状态与磁盘一致。
2. 对准确资产清单运行 `codex-unreal-blueprint inspect` 和 `verify`。
3. 再次检查 Git/SVN 状态，确认只保留预期改动。
4. 在资产清单和哈希核对完成前保留 Job Journal；Journal 只是审计元数据，不是备份。
