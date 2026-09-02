#include "PiUnrealBlueprintCoreModule.h"

#include "Modules/ModuleManager.h"
#include "PiUnrealBlueprintProtocol.h"

DEFINE_LOG_CATEGORY_STATIC(LogPiUnrealBlueprintCore, Log, All);

void FPiUnrealBlueprintCoreModule::StartupModule()
{
    UE_LOG(LogPiUnrealBlueprintCore, Log, TEXT("PiUnrealBlueprint Core %s initialized for protocol %s."),
        PiUnrealBlueprint::PluginVersion, PiUnrealBlueprint::ProtocolVersion);
}

void FPiUnrealBlueprintCoreModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FPiUnrealBlueprintCoreModule, PiUnrealBlueprintCore)
