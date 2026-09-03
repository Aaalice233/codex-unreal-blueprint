#pragma once

#include "Modules/ModuleInterface.h"
#include "Delegates/Delegate.h"

class PIUNREALBLUEPRINTCORE_API FPiUnrealBlueprintCoreModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    bool TickJobs(float DeltaTime);
    FDelegateHandle JobTickerHandle;
};
