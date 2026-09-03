#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "PiUnrealBlueprintActionCatalog.h"

class UBlueprint;

namespace PiUnrealBlueprint
{
    struct PIUNREALBLUEPRINTCORE_API FGraphOperationResult
    {
        bool bChanged = false;
        FString AssetPath;
        FString GraphPath;
        FString GraphGuid;
        TArray<FString> NodeGuids;
        TArray<FString> PinGuids;
        TSharedPtr<FJsonObject> Details;

        TSharedRef<FJsonObject> ToJson() const;
    };

    /** Executes one already registry-validated Graph operation on the game thread. */
    class PIUNREALBLUEPRINTCORE_API FBlueprintGraphOperations
    {
    public:
        static bool Apply(UBlueprint* Blueprint, const TSharedRef<FJsonObject>& Operation,
            FGraphOperationResult& OutResult, FGraphActionError& OutError,
            int32 OperationIndex = INDEX_NONE);

    private:
        FBlueprintGraphOperations() = delete;
    };
}
