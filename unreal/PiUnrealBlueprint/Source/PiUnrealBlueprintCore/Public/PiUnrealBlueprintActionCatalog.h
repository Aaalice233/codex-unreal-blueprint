#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "PiUnrealBlueprintOperationResult.h"

class UBlueprint;
class UBlueprintNodeSpawner;
class UEdGraph;
class UEdGraphNode;

namespace PiUnrealBlueprint
{
    using FGraphActionError = FBlueprintOperationError;

    struct PIUNREALBLUEPRINTCORE_API FGraphActionSearchRequest
    {
        FString Query;
        UBlueprint* Blueprint = nullptr;
        UEdGraph* Graph = nullptr;
        FString Cursor;
        int32 PageSize = 50;
        bool bCompatibleOnly = true;
        int32 OperationIndex = INDEX_NONE;
    };

    struct PIUNREALBLUEPRINTCORE_API FGraphActionSearchResult
    {
        FString CatalogFingerprint;
        FString NextCursor;
        int32 TotalMatches = 0;
        TArray<TSharedPtr<FJsonValue>> Actions;

        TSharedRef<FJsonObject> ToJson() const;
    };

    /**
     * Immutable view of UE4.27's FBlueprintActionDatabase for one request.
     * IDs contain the catalog fingerprint and a hash of the spawner's canonical
     * descriptor; callers can therefore never silently resolve a stale action.
     */
    class PIUNREALBLUEPRINTCORE_API FBlueprintGraphActionCatalog
    {
    public:
        static bool Search(const FGraphActionSearchRequest& Request, FGraphActionSearchResult& OutResult, FGraphActionError& OutError);
        static bool Resolve(const FString& ActionId, UBlueprint* Blueprint, UEdGraph* Graph,
            UBlueprintNodeSpawner*& OutSpawner, FGraphActionError& OutError,
            int32 OperationIndex = INDEX_NONE);
        static bool Spawn(const FString& ActionId, UBlueprint* Blueprint, UEdGraph* Graph,
            const FVector2D& Location, UEdGraphNode*& OutNode, FGraphActionError& OutError,
            int32 OperationIndex = INDEX_NONE);
        static FString GetFingerprint(FGraphActionError& OutError);

    private:
        FBlueprintGraphActionCatalog() = delete;
    };
}
