#pragma once

#include "Modules/ModuleInterface.h"

class PIUNREALBLUEPRINTCORE_API FPiUnrealBlueprintCoreModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
};
