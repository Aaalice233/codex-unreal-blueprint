#if WITH_DEV_AUTOMATION_TESTS

#include "../CodexDocumentImportGuard.h"
#include "CodexInteractiveCSVImportTestFactory.h"

#include "Engine/DataTable.h"
#include "Factories/CSVImportFactory.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "SCSVImportOptions.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

using namespace CodexUnrealBlueprint;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDocumentImportPathTest,
    "CodexUnrealBlueprint.Editor.DocumentImportGuard.Paths",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDocumentImportPathTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("历史 Lua 资源报告"), FDocumentImportGuard::IsGeneratedDocumentPath(
        TEXT("/Game/Lua/_codex_asset_reports/name-g20-niagara-raw")));
    TestTrue(TEXT("历史 Lua 日志报告"), FDocumentImportGuard::IsGeneratedDocumentPath(
        TEXT("/Game/Lua/_codex_log_reports/session/analysis")));
    TestTrue(TEXT("历史 Content 资源报告"), FDocumentImportGuard::IsGeneratedDocumentPath(
        TEXT("/Game/_codex_asset_reports/analysis")));
    TestTrue(TEXT("历史 Content 日志报告"), FDocumentImportGuard::IsGeneratedDocumentPath(
        TEXT("/Game/_codex_log_reports/analysis")));
    for (const FString& Path : {
        FString(TEXT("/Game/Lua/Game20/Config/Data")),
        FString(TEXT("/Game/Lua/ResourceMap/Public/Mat/Data/Data")),
        FString(TEXT("/Game/Lua/_codex_asset_reports_backup/analysis")),
        FString(TEXT("/Game/Lua/_codex_asset_reports")),
        FString(TEXT("/Game/Lua/_codex_asset_reports/")),
        FString(TEXT("/Game/Lua/_codex_asset_reports/../Config/Data")),
        FString(TEXT("/Game/Lua/_codex_asset_reports/./Data")),
        FString(TEXT("/Game/Lua/UserDocuments/analysis")),
        FString(TEXT("/OtherPlugin/Lua/_codex_asset_reports/analysis")),
        FString(TEXT("E:/Codex/_Temp/Master/analysis.json")),
        FString() })
    {
        TestFalse(*FString::Printf(TEXT("保留非目标路径：%s"), *Path),
            FDocumentImportGuard::IsGeneratedDocumentPath(Path));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDocumentImportModalTest,
    "CodexUnrealBlueprint.Editor.DocumentImportGuard.NativeModal",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FDocumentImportModalTest::RunTest(const FString& Parameters)
{
    // FApp::IsUnattended 包含自动化标记，不能用它判断窗口环境。
    if (!FSlateApplication::IsInitialized() || !FSlateApplication::Get().CanDisplayWindows()
        || IsRunningCommandlet() || FParse::Param(FCommandLine::Get(), TEXT("unattended")))
    {
        AddError(TEXT("此测试需要启用原生窗口渲染且未传入 -unattended 的 Editor。"));
        return false;
    }
    const TSharedPtr<SWindow> ExistingModal = FSlateApplication::Get().GetActiveModalWindow();
    if (ExistingModal.IsValid())
    {
        AddError(FString::Printf(TEXT("已有模态窗口阻塞测试：%s"), *ExistingModal->GetTitle().ToString()));
        return false;
    }

    FSlateApplication& Slate = FSlateApplication::Get();
    const FString RunId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const auto CheckFactory = [this, &Slate, &RunId](const FString& Directory, bool bExpectAutoCancel)
    {
        const FString Name = TEXT("ImportGuard_") + RunId;
        UPackage* Package = CreatePackage(*(Directory + TEXT("/") + Name));
        TStrongObjectPtr<UCodexInteractiveCSVImportTestFactory> Factory(
            NewObject<UCodexInteractiveCSVImportTestFactory>());
        TestFalse(TEXT("测试工厂未注册为文件导入器"),
            GetDefault<UCodexInteractiveCSVImportTestFactory>()->bEditorImport);
        bool bDeadlineCancelled = false;
        const double Deadline = FPlatformTime::Seconds() + (bExpectAutoCancel ? 2.0 : 0.25);

        // 超时退出只用于测试防挂死，仍将未自动取消判为失败。
        const FDelegateHandle DeadlineHandle = Slate.GetOnModalLoopTickEvent().AddLambda(
            [&Slate, &bDeadlineCancelled, Deadline](float)
            {
                const TSharedPtr<SWindow> Window = Slate.GetActiveModalWindow();
                if (Window.IsValid() && FPlatformTime::Seconds() >= Deadline
                    && Window->GetContent()->GetType() == FName(TEXT("SCSVImportOptions")))
                {
                    bDeadlineCancelled = true;
                    StaticCastSharedRef<SCSVImportOptions>(
                        ConstCastSharedRef<SWidget>(Window->GetContent()))->OnCancel();
                }
            });

        const FString Json = TEXT("{\"report\":\"inspection\"}");
        const TCHAR* Buffer = *Json;
        bool bCancelled = false;
        UObject* Imported = Factory->FactoryCreateText(UDataTable::StaticClass(), Package,
            FName(*Name), RF_Transient, nullptr, TEXT("json"), Buffer,
            Buffer + Json.Len(), GWarn, bCancelled);
        Slate.GetOnModalLoopTickEvent().Remove(DeadlineHandle);

        TestEqual(*FString::Printf(TEXT("自动取消范围正确：%s"), *Directory),
            bDeadlineCancelled, !bExpectAutoCancel);
        TestTrue(TEXT("原生工厂返回取消"), bCancelled);
        TestNull(TEXT("未导入 DataTable"), Imported);
        TestNull(TEXT("目标 Package 不残留 DataTable"), FindObject<UDataTable>(Package, *Name));
        TestFalse(TEXT("目标 Package 未被修改"), Package->IsDirty());
        Factory->CleanUp();
    };

    CheckFactory(TEXT("/Game/Lua/_codex_asset_reports"), true);
    CheckFactory(TEXT("/Game/Lua/_codex_log_reports"), true);
    CheckFactory(TEXT("/Game/CodexAutomation/") + RunId + TEXT("/Config"), false);

    // 相同标题和报告文字不足以识别导入窗口，必须匹配原生控件。
    const TSharedRef<SWindow> OtherWindow = SNew(SWindow)
        .Title(FText::FromString(TEXT("DataTable Options")))
        .ClientSize(FVector2D(420, 120));
    OtherWindow->SetContent(SNew(STextBlock).Text(
        FText::FromString(TEXT("/Game/Lua/_codex_asset_reports/analysis"))));
    bool bOtherWindowPreserved = false;
    const double OtherDeadline = FPlatformTime::Seconds() + 0.25;
    const FDelegateHandle OtherHandle = Slate.GetOnModalLoopTickEvent().AddLambda(
        [&OtherWindow, &bOtherWindowPreserved, OtherDeadline](float)
        {
            if (FPlatformTime::Seconds() >= OtherDeadline)
            {
                bOtherWindowPreserved = true;
                OtherWindow->RequestDestroyWindow();
            }
        });
    Slate.AddModalWindow(OtherWindow, nullptr);
    Slate.GetOnModalLoopTickEvent().Remove(OtherHandle);
    TestTrue(TEXT("同名非导入窗口仍由用户控制"), bOtherWindowPreserved);
    return true;
}

#endif
