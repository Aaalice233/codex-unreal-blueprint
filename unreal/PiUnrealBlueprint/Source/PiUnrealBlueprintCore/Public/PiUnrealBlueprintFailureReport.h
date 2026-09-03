#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace PiUnrealBlueprint
{
    enum class EWriteAssetState : uint8
    {
        Modified,
        Saved,
        NotSaved,
        Unknown
    };

    enum class EWorkingCopyKind : uint8
    {
        None,
        Git,
        Svn
    };

    struct PIUNREALBLUEPRINTCORE_API FWriteAssetFailureState
    {
        FString PackageName;
        FString Filename;
        EWriteAssetState State = EWriteAssetState::Unknown;
        FString BeforeHash;
        FString LastConfirmedHash;
        bool bSaveAttempted = false;
        bool bSaveSucceeded = false;
        bool bReloadVerified = false;

        TSharedRef<FJsonObject> ToJson() const;
    };

    struct PIUNREALBLUEPRINTCORE_API FWriteFailureReport
    {
        bool bPartial = false;
        bool bStateUnknown = false;
        FString FailedPhase;
        FString Message;
        EWorkingCopyKind WorkingCopy = EWorkingCopyKind::None;
        FString WorkingCopyRoot;
        TArray<FWriteAssetFailureState> Assets;
        TArray<FString> ReadOnlyInspectionCommands;
        TArray<FString> ManualRecoveryAdvice;

        void Finalize(const FString& ProjectDirectory);
        TSharedRef<FJsonObject> ToJson() const;
    };

    PIUNREALBLUEPRINTCORE_API const TCHAR* LexToString(EWriteAssetState State);
    PIUNREALBLUEPRINTCORE_API const TCHAR* LexToString(EWorkingCopyKind Kind);
}
