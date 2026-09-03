#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "PiUnrealBlueprintProtocol.h"

namespace PiUnrealBlueprint
{
    class PIUNREALBLUEPRINTCORE_API FBlueprintSearch
    {
    public:
        static bool Search(const FString& Query, const FString& Domain,
            const TSharedPtr<FJsonObject>& Context, const FString& Cursor, int32 Limit,
            TSharedRef<FJsonObject>& OutResult, FProtocolError& OutError);
    };
}
