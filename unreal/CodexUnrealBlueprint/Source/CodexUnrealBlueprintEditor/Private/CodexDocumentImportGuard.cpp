#include "CodexDocumentImportGuard.h"

#include "Framework/Application/SlateApplication.h"
#include "Layout/Children.h"
#include "Misc/PackageName.h"
#include "SCSVImportOptions.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

DEFINE_LOG_CATEGORY_STATIC(LogCodexDocumentImportGuard, Log, All);

namespace CodexUnrealBlueprint
{
    namespace
    {
        void FindPackageLabels(const TSharedRef<SWidget>& Widget, TArray<FString>& OutPaths)
        {
            if (Widget->GetType() == FName(TEXT("STextBlock")))
            {
                const FString Text = StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString();
                if (Text.StartsWith(TEXT("/Game/")))
                {
                    OutPaths.Add(Text);
                }
            }

            FChildren* Children = Widget->GetChildren();
            for (int32 Index = 0; Children && Index < Children->Num(); ++Index)
            {
                FindPackageLabels(Children->GetChildAt(Index), OutPaths);
            }
        }
    }

    FDocumentImportGuard::~FDocumentImportGuard()
    {
        Stop();
    }

    void FDocumentImportGuard::Start()
    {
        if (!ModalLoopHandle.IsValid() && FSlateApplication::IsInitialized())
        {
            // 模态窗口会阻塞普通 Editor Tick，需监听 Slate 模态循环。
            ModalLoopHandle = FSlateApplication::Get().GetOnModalLoopTickEvent().AddRaw(
                this, &FDocumentImportGuard::TickModalLoop);
        }
    }

    void FDocumentImportGuard::Stop()
    {
        if (ModalLoopHandle.IsValid() && FSlateApplication::IsInitialized())
        {
            FSlateApplication::Get().GetOnModalLoopTickEvent().Remove(ModalLoopHandle);
        }
        ModalLoopHandle.Reset();
        InspectedWindow.Reset();
    }

    bool FDocumentImportGuard::IsGeneratedDocumentPath(const FString& PackagePath)
    {
        if (PackagePath.EndsWith(TEXT("/")) || !FPackageName::IsValidLongPackageName(PackagePath))
        {
            return false;
        }

        // 仅识别历史报告目录，业务配置和同名前缀目录仍由用户导入。
        return PackagePath.StartsWith(TEXT("/Game/Lua/_codex_asset_reports/"))
            || PackagePath.StartsWith(TEXT("/Game/Lua/_codex_log_reports/"))
            || PackagePath.StartsWith(TEXT("/Game/_codex_asset_reports/"))
            || PackagePath.StartsWith(TEXT("/Game/_codex_log_reports/"));
    }

    void FDocumentImportGuard::TickModalLoop(float DeltaTime)
    {
        const TSharedPtr<SWindow> Window = FSlateApplication::Get().GetActiveModalWindow();
        if (!Window.IsValid() || InspectedWindow.Pin() == Window)
        {
            return;
        }
        InspectedWindow = Window;

        const TSharedRef<SWidget> Content = ConstCastSharedRef<SWidget>(Window->GetContent());
        if (Content->GetType() != FName(TEXT("SCSVImportOptions")))
        {
            return;
        }

        TArray<FString> PackagePaths;
        FindPackageLabels(Content, PackagePaths);
        if (PackagePaths.Num() != 1 || !IsGeneratedDocumentPath(PackagePaths[0]))
        {
            return;
        }

        // 调用原生 Cancel，确保工厂收到取消结果而非导入成功。
        StaticCastSharedRef<SCSVImportOptions>(Content)->OnCancel();
        UE_LOG(LogCodexDocumentImportGuard, Display,
            TEXT("已取消 Codex 报告误导入 / Cancelled generated document import: %s"), *PackagePaths[0]);
    }
}
