#include "PiUnrealBlueprintCoreModule.h"

#include "Modules/ModuleManager.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformTime.h"
#include "PiUnrealBlueprintJobs.h"
#include "PiUnrealBlueprintProtocol.h"
#include "PiUnrealBlueprintRequestJournal.h"

DEFINE_LOG_CATEGORY_STATIC(LogPiUnrealBlueprintCore, Log, All);

void FPiUnrealBlueprintCoreModule::StartupModule()
{
    PiUnrealBlueprint::FProtocolError JournalError;
    if (!PiUnrealBlueprint::FRequestJournal::Get().Initialize(JournalError))
    {
        UE_LOG(LogPiUnrealBlueprintCore, Error, TEXT("Request journal startup failed: %s (%s)"),
            *JournalError.Message, *JournalError.UECallsite);
    }
    JobTickerHandle = FTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateRaw(this, &FPiUnrealBlueprintCoreModule::TickJobs), 0.01f);
    UE_LOG(LogPiUnrealBlueprintCore, Log, TEXT("PiUnrealBlueprint Core %s initialized for protocol %s."),
        PiUnrealBlueprint::PluginVersion, PiUnrealBlueprint::ProtocolVersion);
}

void FPiUnrealBlueprintCoreModule::ShutdownModule()
{
    if (JobTickerHandle.IsValid())
    {
        FTicker::GetCoreTicker().RemoveTicker(JobTickerHandle);
        JobTickerHandle.Reset();
    }
    PiUnrealBlueprint::FJobManager::Get().Shutdown();
}

bool FPiUnrealBlueprintCoreModule::TickJobs(float DeltaTime)
{
    PiUnrealBlueprint::FJobManager::Get().Tick(FPlatformTime::Seconds());
    return true;
}

IMPLEMENT_MODULE(FPiUnrealBlueprintCoreModule, PiUnrealBlueprintCore)
