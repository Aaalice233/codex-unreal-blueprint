#include "CodexUnrealBlueprintTransportModule.h"

#include "CoreGlobals.h"
#include "Modules/ModuleManager.h"
#include "CodexUnrealBlueprintTransportServer.h"

DEFINE_LOG_CATEGORY_STATIC(LogCodexUnrealBlueprintTransport, Log, All);

void FCodexUnrealBlueprintTransportModule::StartupModule()
{
    // Commandlets use the Core module directly and must not publish an editor TCP session.
    if (IsRunningCommandlet()) return;

    Server = MakeUnique<CodexUnrealBlueprint::FTransportServer>();
    const CodexUnrealBlueprint::FTransportServerConfig Config;
    if (!Server->Start(Config, LastError))
    {
        UE_LOG(LogCodexUnrealBlueprintTransport, Error, TEXT("%s: %s (%s)"),
            CodexUnrealBlueprint::LexToString(LastError.Code), *LastError.Message, *LastError.UECallsite);
        return;
    }

    LastError = CodexUnrealBlueprint::FProtocolError();
    UE_LOG(LogCodexUnrealBlueprintTransport, Display, TEXT("Listening on 127.0.0.1:%d for editor session %s."),
        Server->GetPort(), *Server->GetEditorSessionId());
}

void FCodexUnrealBlueprintTransportModule::ShutdownModule()
{
    if (Server.IsValid())
    {
        Server->Stop();
        Server.Reset();
    }
}

IMPLEMENT_MODULE(FCodexUnrealBlueprintTransportModule, CodexUnrealBlueprintTransport)
