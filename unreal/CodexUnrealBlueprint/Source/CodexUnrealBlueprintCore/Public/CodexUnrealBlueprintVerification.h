#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "CodexUnrealBlueprintProtocol.h"

namespace CodexUnrealBlueprint
{
    class CODEXUNREALBLUEPRINTCORE_API FBlueprintVerification
    {
    public:
        static bool Verify(const TArray<FString>& AssetPaths,
            const TArray<TSharedPtr<FJsonValue>>& Expectations, bool bCompile, bool bReload,
            TSharedRef<FJsonObject>& OutResult, FProtocolError& OutError);
    };
}
