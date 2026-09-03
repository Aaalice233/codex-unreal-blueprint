#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "PiUnrealBlueprintProtocol.h"

namespace PiUnrealBlueprint
{
    class PIUNREALBLUEPRINTCORE_API FBlueprintVerification
    {
    public:
        static bool Verify(const TArray<FString>& AssetPaths,
            const TArray<TSharedPtr<FJsonValue>>& Expectations, bool bCompile, bool bReload,
            TSharedRef<FJsonObject>& OutResult, FProtocolError& OutError);
    };
}
