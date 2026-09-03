#pragma once

#include "Modules/ModuleInterface.h"
#include "CodexUnrealBlueprintProtocol.h"

namespace CodexUnrealBlueprint
{
    class FTransportServer;
}

class CODEXUNREALBLUEPRINTTRANSPORT_API FCodexUnrealBlueprintTransportModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    TUniquePtr<CodexUnrealBlueprint::FTransportServer> Server;
    CodexUnrealBlueprint::FProtocolError LastError;
};
