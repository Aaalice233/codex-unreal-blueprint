#include "PiUnrealBlueprintEditorModule.h"

#include "LevelEditor.h"
#include "Misc/App.h"
#include "Modules/ModuleManager.h"
#include "PiUnrealBlueprintTransportModule.h"

#define LOCTEXT_NAMESPACE "PiUnrealBlueprintEditor"

const FName FPiUnrealBlueprintEditorModule::StatusBarItemName(TEXT("PiUnrealBlueprint.Status"));

void FPiUnrealBlueprintEditorModule::StartupModule()
{
    if (IsRunningCommandlet() || FApp::IsUnattended())
    {
        return;
    }

    FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
    FLevelEditorModule::FStatusBarItem StatusItem;
    StatusItem.Visibility = EVisibility::Visible;
    StatusItem.Label = LOCTEXT("StatusLabel", "Pi Blueprint: ");
    StatusItem.Value = LOCTEXT("StatusUnavailable", "Unavailable (NotImplemented)");
    LevelEditorModule.AddStatusBarItem(StatusBarItemName, StatusItem);
    LevelEditorModule.BroadcastNotificationBarChanged();
    bStatusBarItemRegistered = true;
}

void FPiUnrealBlueprintEditorModule::ShutdownModule()
{
    if (bStatusBarItemRegistered && FModuleManager::Get().IsModuleLoaded(TEXT("LevelEditor")))
    {
        FLevelEditorModule& LevelEditorModule = FModuleManager::GetModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
        LevelEditorModule.RemoveStatusBarItem(StatusBarItemName);
        LevelEditorModule.BroadcastNotificationBarChanged();
    }
}

IMPLEMENT_MODULE(FPiUnrealBlueprintEditorModule, PiUnrealBlueprintEditor)

#undef LOCTEXT_NAMESPACE
