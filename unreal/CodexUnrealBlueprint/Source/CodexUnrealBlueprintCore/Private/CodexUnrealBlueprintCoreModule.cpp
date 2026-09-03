#include "CodexUnrealBlueprintCoreModule.h"

#include "Modules/ModuleManager.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformTime.h"
#include "CodexUnrealBlueprintJobs.h"
#include "CodexUnrealBlueprintProtocol.h"
#include "CodexUnrealBlueprintRequestJournal.h"

DEFINE_LOG_CATEGORY_STATIC(LogCodexUnrealBlueprintCore, Log, All);

void FCodexUnrealBlueprintCoreModule::StartupModule()
{
    CodexUnrealBlueprint::FProtocolError JournalError;
    if (!CodexUnrealBlueprint::FRequestJournal::Get().Initialize(JournalError))
    {
        UE_LOG(LogCodexUnrealBlueprintCore, Error, TEXT("Request journal startup failed: %s (%s)"),
            *JournalError.Message, *JournalError.UECallsite);
    }
    JobTickerHandle = FTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateRaw(this, &FCodexUnrealBlueprintCoreModule::TickJobs), 0.01f);
    UE_LOG(LogCodexUnrealBlueprintCore, Log, TEXT("CodexUnrealBlueprint Core %s initialized for protocol %s."),
        CodexUnrealBlueprint::PluginVersion, CodexUnrealBlueprint::ProtocolVersion);
}

void FCodexUnrealBlueprintCoreModule::ShutdownModule()
{
    if (JobTickerHandle.IsValid())
    {
        FTicker::GetCoreTicker().RemoveTicker(JobTickerHandle);
        JobTickerHandle.Reset();
    }
    CodexUnrealBlueprint::FJobManager::Get().Shutdown();
}

bool FCodexUnrealBlueprintCoreModule::TickJobs(float DeltaTime)
{
    CodexUnrealBlueprint::FJobManager::Get().Tick(FPlatformTime::Seconds());
    return true;
}

IMPLEMENT_MODULE(FCodexUnrealBlueprintCoreModule, CodexUnrealBlueprintCore)
