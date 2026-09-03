#include "PiUnrealBlueprintTransportModule.h"

#include "CoreGlobals.h"
#include "Modules/ModuleManager.h"
#include "PiUnrealBlueprintTransportServer.h"

DEFINE_LOG_CATEGORY_STATIC(LogPiUnrealBlueprintTransport, Log, All);

void FPiUnrealBlueprintTransportModule::StartupModule()
{
    // Commandlets use the Core module directly and must not publish an editor TCP session.
    if (IsRunningCommandlet()) return;

    Server = MakeUnique<PiUnrealBlueprint::FTransportServer>();
    const PiUnrealBlueprint::FTransportServerConfig Config;
    if (!Server->Start(Config, LastError))
    {
        UE_LOG(LogPiUnrealBlueprintTransport, Error, TEXT("%s: %s (%s)"),
            PiUnrealBlueprint::LexToString(LastError.Code), *LastError.Message, *LastError.UECallsite);
        return;
    }

    LastError = PiUnrealBlueprint::FProtocolError();
    UE_LOG(LogPiUnrealBlueprintTransport, Display, TEXT("Listening on 127.0.0.1:%d for editor session %s."),
        Server->GetPort(), *Server->GetEditorSessionId());
}

void FPiUnrealBlueprintTransportModule::ShutdownModule()
{
    if (Server.IsValid())
    {
        Server->Stop();
        Server.Reset();
    }
}

IMPLEMENT_MODULE(FPiUnrealBlueprintTransportModule, PiUnrealBlueprintTransport)
