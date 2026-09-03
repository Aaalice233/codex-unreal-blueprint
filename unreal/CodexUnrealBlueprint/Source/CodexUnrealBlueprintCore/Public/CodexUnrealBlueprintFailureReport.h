#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace CodexUnrealBlueprint
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

    struct CODEXUNREALBLUEPRINTCORE_API FWriteAssetFailureState
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

    struct CODEXUNREALBLUEPRINTCORE_API FWriteFailureReport
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

    CODEXUNREALBLUEPRINTCORE_API const TCHAR* LexToString(EWriteAssetState State);
    CODEXUNREALBLUEPRINTCORE_API const TCHAR* LexToString(EWorkingCopyKind Kind);
}
