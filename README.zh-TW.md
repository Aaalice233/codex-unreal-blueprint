<p align="center">
  <img src="Docs/images/readme-banner.png" alt="Codex Unreal Blueprint 橫幅" width="100%">
</p>

# Codex Unreal Blueprint

[简体中文](README.md) · **繁體中文** · [English](README.en.md)

專為 Codex 打造的 UE4.27 Win64 本機外掛：Editor 開啟時可安全自動化 Blueprint，Editor 關閉時也能直接檢查 `.uasset` 和 `.umap`。Skill、MCP server、UE Editor plugin 與離線解析器一次安裝，不必另外安裝 `inspect-unreal-uassets`。

## ✨ 主要功能

- 在線或離線檢查 Unreal 資產，並清楚區分 `generic`、`specialized`、`editable` 三層能力。
- 建立、複製、移動、重新命名、編輯和刪除常用 Blueprint 資產。
- 編輯元件、變數、Graph、節點、Pin、連線、UMG WidgetTree 與 AnimBlueprint。
- 深度讀取 Blueprint、UMG、AnimBlueprint、AnimMontage、Material、Material Instance 和 Niagara System。
- 比較兩個資產並查詢相依與引用；在線使用 Asset Registry，離線提供序列化證據。
- 寫入前預檢，寫入後編譯、儲存、重新載入並驗證；失敗時保留準確資產清單與原始編譯資訊。

## 🧭 在線、離線與自動路由

| 模式 | 適合情境 | 行為 |
|---|---|---|
| `auto` | 日常使用，建議 | 有唯一匹配 Editor 時在線處理；沒有匹配會話且提供檔案路徑時轉為離線解析 |
| `editor` | 當前未儲存狀態、精確引用、Blueprint 寫入 | 連接經驗證的 UE Editor plugin，讀取真實 UObject 與 Asset Registry 資料 |
| `offline` | Editor 已關閉、資產只適合靜態解析、需要磁碟證據 | 使用隨包安裝的 UAssetAPI 解析器；唯讀且不宣稱為執行階段結果 |

對於可能被 Editor 佔用，或更適合離線處理的特效等資產，可啟用：

```json
{
  "mode": "offline",
  "filePath": "E:/Project/Content/Effects/NS_Test.uasset",
  "contentRoot": "E:/Project/Content",
  "offlineStaging": {
    "enabled": true,
    "maxCachedAssets": 64
  }
}
```

解析器會複製目標 Package 與現有的 `.uexp`、`.ubulk`、`.uptnl` companion 檔案，確認來源在複製期間保持穩定，再解析隔離副本。快取只按主 `.uasset/.umap` 數量計算；達到上限後會自動淘汰最舊快照並繼續工作，不會因歷史快取已滿而拒絕新資產。

## 🧩 資產支援範圍

| 資產類型 | 專用讀取 | 在線編輯 |
|---|---:|---:|
| 一般 Blueprint、Actor、ActorComponent、Interface、Function/Macro Library | ✅ | ✅ |
| UMG / Widget Blueprint | ✅ WidgetTree、Graph、動畫、屬性 | ✅ 現有資產 |
| Animation Blueprint | ✅ AnimGraph、狀態機、變數 | ✅ 現有資產 |
| User Defined Struct / Enum、Level Blueprint | ✅ | ✅ |
| AnimMontage | ✅ Section、Slot、Notify、Blend | 唯讀 |
| Material / Material Instance | ✅ 參數、父材質、Expression | 唯讀 |
| Niagara System | ✅ 參數、Emitter、Warmup 與序列化證據 | 唯讀 |
| Texture、Mesh、Sound、DataAsset、Sequence 等其他可載入資產 | 一般屬性、相依、引用、Imports/Exports | 唯讀 |

目前可直接建立一般 Blueprint、Interface、Function Library、Macro Library、Struct 和 Enum。現有 UMG 與 AnimBlueprint 可深入編輯；專用建立入口尚未公開。

## 🛠️ Blueprint 自動化

- **資產**：建立、複製、移動、重新命名、刪除、變更父類別、增刪 Interface、修改 Class Defaults。
- **元件**：新增、移除、重新命名、掛接、設定 Root、Transform、屬性和繼承覆寫。
- **型別**：新增、更新、刪除變數；編輯 Struct 欄位與 Enum 值。
- **Graph**：建立或刪除 Graph，編輯函式簽章、區域變數、Dispatcher、節點、Pin 和連線。
- **UMG**：編輯控制項層級、Named Slot、屬性、事件、Binding、導覽、無障礙與時間軸動畫。
- **AnimBlueprint**：編輯 Skeleton、父類別、AnimGraph 節點、狀態機、State、Conduit、Transition、Pose Link 與 Event Graph。

具體 operation 與參數由執行中的 UE plugin Operation Registry 動態提供，Skill 不會猜測不存在的節點或欄位。

## 🛡️ 寫入安全

每次寫入都需要唯一 `requestId`，並依序執行：

```text
嚴格預檢 → UE Transaction → 修改 → 編譯 → 儲存 → 重新載入 → 結構驗證
```

- 目標 Package 已 Dirty、Source Control Checkout 失敗、Schema 不符或編譯失敗時明確停止。
- 連線狀態不明時查詢原 `requestId`，不會盲目重播寫入。
- 部分失敗會回傳 `modified`、`saved`、`notSaved`、`unknown` 等準確清單。
- 外掛不自動備份或還原二進位資產；Git/SVN 復原由使用者依清單手動執行。

## 🚀 安裝

需求：Windows、UE4.27、PowerShell 7、Node.js 22.19+、.NET SDK 8+、Visual Studio C++ 工具鏈，以及 Codex Desktop/CLI。

首次安裝或 UE plugin 有更新時，先關閉目標 Editor，再執行：

```powershell
npm install
pwsh ./scripts/setup.ps1 `
  -UProject E:/Project/MyGame.uproject `
  -EngineRoot E:/UE_4.27
```

腳本會執行檢查、建置 UE4.27 Win64 plugin、同步受管檔案，並安裝個人 Codex plugin。完成後重新啟動 Editor，並建立新的 Codex task。

若 UE plugin 已是最新版，本次只更新 Skill、MCP server 或離線解析器，可以保持 Editor 開啟：

```powershell
pwsh ./scripts/setup.ps1 -CodexOnly
```

此時不必重新啟動 Editor，只需建立新的 Codex task。自動找不到 CLI 時再傳入 `-CodexExecutable C:/path/to/codex.exe`。

## 💬 使用範例

- 「檢查這個 UMG 的 WidgetTree 和按鈕事件。」
- 「比較這兩個 Niagara，看看參數與 Emitter 有什麼差異。」
- 「為這個 Actor Blueprint 加入元件、變數和初始化節點並驗證。」
- 「不要關閉 Editor，把這個特效複製到離線快取後解析。」
- 「查出誰引用了這個 Blueprint，並區分在線與離線證據。」

## 🧰 MCP 工具

- **環境與搜尋**：`unreal_status`、`unreal_doctor`、`unreal_search`
- **一般資產**：`unreal_asset_inspect`、`unreal_asset_compare`、`unreal_asset_referencers`
- **Blueprint 工作流程**：`blueprint_capabilities`、`blueprint_inspect`、`blueprint_validate`、`blueprint_apply`、`blueprint_job`、`blueprint_verify`

## 📚 文件

- [安裝與本機開發](Docs/setup.zh-CN.md)
- [架構與能力分層](Docs/architecture.zh-CN.md)
- [MCP 工具參考](Docs/mcp-reference.zh-CN.md)
- [Git/SVN 手動復原](Docs/source-control-recovery.zh-CN.md)
- [v1.0.0 發布門檻](Docs/v1-release-gate.zh-CN.md)

## ⚠️ 使用邊界

- 離線結果是磁碟序列化證據，不能證明 Construction Script、Lua/C++ 或執行階段修改後的最終狀態。
- Cooked、unversioned、損壞或高度自訂序列化的 Package 可能只能部分解析。
- Material、Niagara、AnimMontage 和其他非 Blueprint 資產目前以檢查、比較及引用分析為主，不執行寫入。
- 支援目標為 UE4.27 Win64；其他引擎版本尚未宣告相容。

## 🤝 開發與授權

```powershell
npm run check
```

貢獻前請閱讀 [CONTRIBUTING.md](CONTRIBUTING.md) 和 [SECURITY.md](SECURITY.md)。專案採用 [MIT License](LICENSE)，離線解析器的第三方聲明請見 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
