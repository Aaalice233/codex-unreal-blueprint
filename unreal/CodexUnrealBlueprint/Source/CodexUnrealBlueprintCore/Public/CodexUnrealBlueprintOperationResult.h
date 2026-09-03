#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace CodexUnrealBlueprint
{
    struct CODEXUNREALBLUEPRINTCORE_API FBlueprintOperationError
    {
        FString Code;
        FString Message;
        FString AssetPath;
        FString GraphPath;
        int32 OperationIndex = INDEX_NONE;
        FString UECallsite;
        TArray<FString> Details;
        TArray<FString> Candidates;

        bool IsSet() const { return !Code.IsEmpty(); }
        TSharedRef<FJsonObject> ToJson() const;
    };

    struct CODEXUNREALBLUEPRINTCORE_API FBlueprintOperationResult
    {
        bool bSuccess = false;
        bool bChanged = false;
        TArray<FString> AffectedAssets;
        TSharedPtr<FJsonObject> Data;
        TOptional<FBlueprintOperationError> Error;

        static FBlueprintOperationResult Success(const TArray<FString>& InAffectedAssets, bool bInChanged = true);
        static FBlueprintOperationResult Failure(const FString& Code, const FString& Message,
            const FString& AssetPath, int32 OperationIndex, const FString& UECallsite,
            const TArray<FString>& Details = TArray<FString>());
        TSharedRef<FJsonObject> ToJson() const;
    };
}
