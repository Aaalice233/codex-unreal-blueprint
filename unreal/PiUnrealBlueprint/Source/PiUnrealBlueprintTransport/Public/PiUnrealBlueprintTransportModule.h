#pragma once

#include "Modules/ModuleInterface.h"
#include "PiUnrealBlueprintProtocol.h"

namespace PiUnrealBlueprint
{
    class FTransportServer;
}

class PIUNREALBLUEPRINTTRANSPORT_API FPiUnrealBlueprintTransportModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    TUniquePtr<PiUnrealBlueprint::FTransportServer> Server;
    PiUnrealBlueprint::FProtocolError LastError;
};
