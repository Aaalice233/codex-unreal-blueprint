#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "CodexUnrealBlueprintProtocol.h"

namespace CodexUnrealBlueprint
{
    class CODEXUNREALBLUEPRINTCORE_API FBlueprintInspection
    {
    public:
        static bool Inspect(const FString& AssetPath, const TArray<FString>& Facets,
            int32 Offset, int32 Limit, TSharedRef<FJsonObject>& OutResult, FProtocolError& OutError);
        static bool Inspect(const FString& AssetPath, const TArray<FString>& Facets,
            const TArray<FString>& ClassDefaultPropertyPaths, int32 Offset, int32 Limit,
            TSharedRef<FJsonObject>& OutResult, FProtocolError& OutError);
        static bool Inspect(const FString& AssetPath, const TArray<FString>& Facets,
            const TArray<FString>& ClassDefaultPropertyPaths, const TSharedPtr<FJsonObject>& ComponentQuery,
            int32 Offset, int32 Limit, TSharedRef<FJsonObject>& OutResult, FProtocolError& OutError);
        static bool ComputeStructureHash(const FString& AssetPath, FString& OutHash, FProtocolError& OutError);
    };
}
