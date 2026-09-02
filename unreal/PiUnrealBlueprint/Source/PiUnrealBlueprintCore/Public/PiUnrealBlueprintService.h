#pragma once

#include "CoreMinimal.h"
#include "PiUnrealBlueprintProtocol.h"

namespace PiUnrealBlueprint
{
    class PIUNREALBLUEPRINTCORE_API FCoreService
    {
    public:
        static FCoreService& Get();

        FProtocolResponse Dispatch(const FProtocolRequest& Request) const;
        FProtocolResponse MakeNotImplemented(const FString& RequestId, const FString& Method) const;

    private:
        FProtocolResponse GetStatus(const FProtocolRequest& Request) const;
    };
}
