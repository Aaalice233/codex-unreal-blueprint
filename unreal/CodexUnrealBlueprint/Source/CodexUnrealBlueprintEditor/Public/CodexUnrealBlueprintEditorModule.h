#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

class FExtender;

namespace CodexUnrealBlueprint
{
    struct FEditorStatusState;
}

class CODEXUNREALBLUEPRINTEDITOR_API FCodexUnrealBlueprintEditorModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    TSharedPtr<FExtender> StatusBarExtender;
    TSharedPtr<CodexUnrealBlueprint::FEditorStatusState, ESPMode::ThreadSafe> StatusState;
    FDelegateHandle StatusTickerHandle;
};
