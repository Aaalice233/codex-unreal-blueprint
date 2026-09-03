#include "PiUnrealBlueprintEditorModule.h"

#include "Containers/Ticker.h"
#include "EditorStyleSet.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "LevelEditor.h"
#include "Misc/App.h"
#include "Modules/ModuleManager.h"
#include "PiUnrealBlueprintJobs.h"
#include "PiUnrealBlueprintProtocol.h"
#include "PiUnrealBlueprintRuntimeStatus.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBorder.h"

#define LOCTEXT_NAMESPACE "PiUnrealBlueprintEditor"

namespace PiUnrealBlueprint
{
    struct FEditorStatusState
    {
        mutable FCriticalSection Mutex;
        FString ActiveJobId;
        FString ActiveJobPhase;
        FString ActiveLease;
        FString LastFailedJobId;
        FString LastFailure;
        TWeakPtr<SBorder> StatusBorder;
        TWeakPtr<SImage> StatusImage;
    };
}

namespace
{
    typedef TSharedPtr<PiUnrealBlueprint::FEditorStatusState, ESPMode::ThreadSafe> FStatusStatePtr;
    typedef TWeakPtr<PiUnrealBlueprint::FEditorStatusState, ESPMode::ThreadSafe> FStatusStateWeakPtr;

    FText GetStatusTooltip(const FStatusStatePtr& State)
    {
        const PiUnrealBlueprint::FRuntimeTransportStatus TransportStatus =
            PiUnrealBlueprint::FRuntimeStatusRegistry::Get().GetTransportStatus();
        const PiUnrealBlueprint::EServiceState ServiceState = TransportStatus.State;

        FScopeLock Lock(&State->Mutex);
        const bool bClientConnected = ServiceState == PiUnrealBlueprint::EServiceState::Connected
            || ServiceState == PiUnrealBlueprint::EServiceState::Busy;
        const FString JobText = State->ActiveJobId.IsEmpty()
            ? TEXT("None")
            : FString::Printf(TEXT("%s (%s)"), *State->ActiveJobId, *State->ActiveJobPhase);
        const FString FailedText = State->LastFailedJobId.IsEmpty()
            ? TEXT("None")
            : FString::Printf(TEXT("%s (%s)"), *State->LastFailedJobId, *State->LastFailure);
        FString Tooltip = FString::Printf(
            TEXT("Pi Unreal Blueprint\nService: %s\nClient: %s\nLease: %s\nJob: %s\nFailed: %s"),
            PiUnrealBlueprint::LexToString(ServiceState),
            bClientConnected ? TEXT("Connected") : TEXT("Disconnected"),
            State->ActiveLease.IsEmpty() ? TEXT("Free") : *State->ActiveLease,
            *JobText,
            *FailedText);
        if (TransportStatus.Error.Code != PiUnrealBlueprint::EErrorCode::None)
        {
            Tooltip += FString::Printf(TEXT("\nService error: %s: %s"),
                PiUnrealBlueprint::LexToString(TransportStatus.Error.Code), *TransportStatus.Error.Message);
        }
        return FText::FromString(Tooltip);
    }

    const FSlateBrush* GetStatusBrush(const FStatusStatePtr& State)
    {
        const PiUnrealBlueprint::EServiceState ServiceState =
            PiUnrealBlueprint::FRuntimeStatusRegistry::Get().GetTransportStatus().State;

        FScopeLock Lock(&State->Mutex);
        if (ServiceState == PiUnrealBlueprint::EServiceState::Faulted || !State->LastFailedJobId.IsEmpty())
        {
            return FEditorStyle::GetBrush(TEXT("Icons.Error"));
        }
        if (!State->ActiveJobId.IsEmpty())
        {
            return FEditorStyle::GetBrush(TEXT("Icons.Warning"));
        }
        return FEditorStyle::GetBrush(TEXT("Icons.Info"));
    }

    void RefreshStatusWidget(const FStatusStatePtr& State)
    {
        TSharedPtr<SBorder> StatusBorder;
        TSharedPtr<SImage> StatusImage;
        {
            FScopeLock Lock(&State->Mutex);
            StatusBorder = State->StatusBorder.Pin();
            StatusImage = State->StatusImage.Pin();
        }

        if (StatusBorder.IsValid())
        {
            StatusBorder->SetToolTipText(GetStatusTooltip(State));
        }
        if (StatusImage.IsValid())
        {
            StatusImage->SetImage(GetStatusBrush(State));
        }
    }

    void AddStatusWidget(FToolBarBuilder& Builder, const FStatusStatePtr& State)
    {
        TSharedPtr<SBorder> StatusBorder;
        TSharedPtr<SImage> StatusImage;
        Builder.AddWidget(
            SAssignNew(StatusBorder, SBorder)
            .BorderImage(FEditorStyle::GetBrush(TEXT("NoBorder")))
            .Padding(FMargin(4.0f, 1.0f))
            [
                SAssignNew(StatusImage, SImage)
            ]);

        {
            FScopeLock Lock(&State->Mutex);
            State->StatusBorder = StatusBorder;
            State->StatusImage = StatusImage;
        }
        RefreshStatusWidget(State);
    }

    bool TickStatus(const FStatusStatePtr& State)
    {
        TArray<PiUnrealBlueprint::FJobSnapshot> Snapshots;
        PiUnrealBlueprint::FJobManager::Get().GetSnapshots(Snapshots);

        FString NewActiveJobId;
        FString NewActiveJobPhase;
        FDateTime LatestActiveUpdate = FDateTime::MinValue();
        FDateTime LatestTerminalUpdate = FDateTime::MinValue();
        TOptional<PiUnrealBlueprint::FJobSnapshot> LatestTerminalJob;
        int32 ActiveReadCount = 0;
        bool bWriteLeaseHeld = false;
        for (const PiUnrealBlueprint::FJobSnapshot& Snapshot : Snapshots)
        {
            if (Snapshot.bTerminal)
            {
                if (Snapshot.UpdatedAt > LatestTerminalUpdate)
                {
                    LatestTerminalUpdate = Snapshot.UpdatedAt;
                    LatestTerminalJob = Snapshot;
                }
                continue;
            }

            if (Snapshot.UpdatedAt > LatestActiveUpdate)
            {
                LatestActiveUpdate = Snapshot.UpdatedAt;
                NewActiveJobId = Snapshot.JobId;
                NewActiveJobPhase = PiUnrealBlueprint::LexToString(Snapshot.Phase);
            }
            if (Snapshot.Phase != PiUnrealBlueprint::EJobPhase::Queued)
            {
                if (Snapshot.Access == PiUnrealBlueprint::EJobAccess::Write)
                {
                    bWriteLeaseHeld = true;
                }
                else
                {
                    ++ActiveReadCount;
                }
            }
        }

        {
            FScopeLock Lock(&State->Mutex);
            if (LatestTerminalJob.IsSet())
            {
                const PiUnrealBlueprint::FJobSnapshot& Snapshot = LatestTerminalJob.GetValue();
                if (Snapshot.Phase == PiUnrealBlueprint::EJobPhase::Failed)
                {
                    State->LastFailedJobId = Snapshot.JobId;
                    State->LastFailure = Snapshot.Error.IsSet()
                        ? FString::Printf(TEXT("%s: %s"),
                            PiUnrealBlueprint::LexToString(Snapshot.Error.GetValue().Code),
                            *Snapshot.Error.GetValue().Message)
                        : TEXT("Job failed without a structured error.");
                }
                else if (Snapshot.Phase == PiUnrealBlueprint::EJobPhase::Succeeded)
                {
                    State->LastFailedJobId.Reset();
                    State->LastFailure.Reset();
                }
            }
            State->ActiveJobId = MoveTemp(NewActiveJobId);
            State->ActiveJobPhase = MoveTemp(NewActiveJobPhase);
            State->ActiveLease = bWriteLeaseHeld
                ? TEXT("Write")
                : (ActiveReadCount > 0 ? FString::Printf(TEXT("Read (%d)"), ActiveReadCount) : TEXT("Free"));
        }

        RefreshStatusWidget(State);
        return true;
    }
}

void FPiUnrealBlueprintEditorModule::StartupModule()
{
    if (IsRunningCommandlet() || FApp::IsUnattended())
    {
        return;
    }

    StatusState = MakeShared<PiUnrealBlueprint::FEditorStatusState, ESPMode::ThreadSafe>();
    const FStatusStateWeakPtr WeakStatusState = StatusState;

    FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
    StatusBarExtender = MakeShared<FExtender>();
    StatusBarExtender->AddToolBarExtension(
        TEXT("Start"),
        EExtensionHook::After,
        nullptr,
        FToolBarExtensionDelegate::CreateLambda([WeakStatusState](FToolBarBuilder& Builder)
        {
            const FStatusStatePtr State = WeakStatusState.Pin();
            if (State.IsValid())
            {
                AddStatusWidget(Builder, State);
            }
        }));
    LevelEditorModule.GetNotificationBarExtensibilityManager()->AddExtender(StatusBarExtender);

    StatusTickerHandle = FTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda([WeakStatusState](float)
        {
            const FStatusStatePtr State = WeakStatusState.Pin();
            return State.IsValid() && TickStatus(State);
        }),
        0.25f);

    // 扩展注册后立即重建状态栏，避免等待下次布局变化。
    LevelEditorModule.BroadcastNotificationBarChanged();
}

void FPiUnrealBlueprintEditorModule::ShutdownModule()
{
    if (StatusTickerHandle.IsValid())
    {
        FTicker::GetCoreTicker().RemoveTicker(StatusTickerHandle);
        StatusTickerHandle.Reset();
    }
    if (StatusBarExtender.IsValid() && FModuleManager::Get().IsModuleLoaded(TEXT("LevelEditor")))
    {
        FLevelEditorModule& LevelEditorModule = FModuleManager::GetModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
        LevelEditorModule.GetNotificationBarExtensibilityManager()->RemoveExtender(StatusBarExtender);
        LevelEditorModule.BroadcastNotificationBarChanged();
    }

    // Slate 控件只保存静态值；先移除所有外部回调，再释放共享状态。
    StatusBarExtender.Reset();
    StatusState.Reset();
}

IMPLEMENT_MODULE(FPiUnrealBlueprintEditorModule, PiUnrealBlueprintEditor)

#undef LOCTEXT_NAMESPACE
