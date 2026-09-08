#pragma once

#include "CoreMinimal.h"
#include "CodexUnrealBlueprintProtocol.h"

namespace CodexUnrealBlueprint
{
    /** Editor presentation only: these operations never modify or save an asset. */
    class CODEXUNREALBLUEPRINTCORE_API FViewportService
    {
    public:
        static TSharedRef<FJsonObject> List();
        static bool Capture(const TSharedRef<FJsonObject>& Params, TSharedPtr<FJsonObject>& Result, FProtocolError& Error);
        static bool Control(const TSharedRef<FJsonObject>& Params, TSharedPtr<FJsonObject>& Result, FProtocolError& Error);
    };
}
