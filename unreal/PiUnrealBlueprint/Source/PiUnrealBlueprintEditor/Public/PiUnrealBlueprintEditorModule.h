#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

class FExtender;

namespace PiUnrealBlueprint
{
    struct FEditorStatusState;
}

class PIUNREALBLUEPRINTEDITOR_API FPiUnrealBlueprintEditorModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    TSharedPtr<FExtender> StatusBarExtender;
    TSharedPtr<PiUnrealBlueprint::FEditorStatusState, ESPMode::ThreadSafe> StatusState;
    FDelegateHandle StatusTickerHandle;
};
