#pragma once

#include "Modules/ModuleInterface.h"

class PIUNREALBLUEPRINTEDITOR_API FPiUnrealBlueprintEditorModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    static const FName StatusBarItemName;
    bool bStatusBarItemRegistered = false;
};
