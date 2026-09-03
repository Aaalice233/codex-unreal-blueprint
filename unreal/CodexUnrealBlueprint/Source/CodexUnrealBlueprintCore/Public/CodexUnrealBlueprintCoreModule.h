#pragma once

#include "Modules/ModuleInterface.h"
#include "Delegates/Delegate.h"

class CODEXUNREALBLUEPRINTCORE_API FCodexUnrealBlueprintCoreModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    bool TickJobs(float DeltaTime);
    FDelegateHandle JobTickerHandle;
};
