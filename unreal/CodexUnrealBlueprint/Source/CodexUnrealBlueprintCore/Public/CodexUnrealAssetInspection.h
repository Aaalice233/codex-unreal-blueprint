#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "CodexUnrealBlueprintProtocol.h"

namespace CodexUnrealBlueprint
{
    class CODEXUNREALBLUEPRINTCORE_API FUnrealAssetInspection
    {
    public:
        static bool Inspect(const FString& AssetPath, const TArray<FString>& Facets,
            const TArray<FString>& PropertyPaths, int32 Offset, int32 Limit,
            TSharedRef<FJsonObject>& OutResult, FProtocolError& OutError);

        static bool Compare(const FString& BaseAssetPath, const FString& TargetAssetPath,
            const TArray<FString>& Facets, const TArray<FString>& PropertyPaths,
            int32 Offset, int32 Limit, TSharedRef<FJsonObject>& OutResult, FProtocolError& OutError);

        static bool FindReferencers(const FString& AssetPath, bool bRecursive, int32 Offset, int32 Limit,
            TSharedRef<FJsonObject>& OutResult, FProtocolError& OutError);
    };
}
