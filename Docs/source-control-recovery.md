# Source control and manual asset recovery

[简体中文](source-control-recovery.zh-CN.md)

## Product boundary

`codex-unreal-blueprint` does **not** copy or back up Unreal packages and does **not** automatically run Git/SVN restore commands. `FScopedTransaction` can undo an in-memory failure before save; it is not a disk backup and cannot recover a crash or partial save.

For a partial save, Editor crash, or unknown connection state, the Job result and local Journal identify each affected package as `modified`, `saved`, `notSaved`, or `unknown`, together with the operation index, phase, and last confirmed hash. The tool reports `partial` or `stateUnknown`; it must not claim success.

## Before changing assets

1. Commit or otherwise record the current Git/SVN working-copy state.
2. Save unrelated Editor work. A dirty target package is rejected.
3. Confirm the Unreal Source Control Provider status. Checkout failure stops the job.
4. Keep unrelated local asset modifications separate so a manual restore cannot overwrite them.

## Git: inspect, then restore manually

Replace the sample paths with the exact package list from the failed Job. Unreal package paths such as `/Game/UI/WBP_Menu` normally map to project-relative files such as `Content/UI/WBP_Menu.uasset`.

```powershell
git status --short -- Content/UI/WBP_Menu.uasset
git diff --stat -- Content/UI/WBP_Menu.uasset
git diff --numstat -- Content/UI/WBP_Menu.uasset
```

After confirming that the file contains only the unwanted job change:

```powershell
git restore --source=HEAD -- Content/UI/WBP_Menu.uasset
```

For a newly created untracked asset, inspect it and remove it manually only if it belongs exclusively to the failed job. The plugin never runs `git restore`, `git checkout`, `git reset`, `git clean`, or file deletion for you.

## SVN: inspect, then revert manually

```powershell
svn status Content/UI/WBP_Menu.uasset
svn diff --summarize Content/UI/WBP_Menu.uasset
svn info Content/UI/WBP_Menu.uasset
```

After confirming the file contains only the unwanted job change:

```powershell
svn revert Content/UI/WBP_Menu.uasset
```

For added files, inspect the scheduled state before using `svn revert` or deleting the local file. The plugin never runs `svn revert`, changes changelists, or deletes files for you.

## After manual recovery

1. Restart/reload the affected package or Editor so memory matches disk.
2. Run `codex-unreal-blueprint inspect` and `verify` against the exact asset set.
3. Run Git/SVN status again and confirm only intended changes remain.
4. Keep the Job Journal until the asset list and hashes are reconciled; the Journal is audit metadata, not a backup.
