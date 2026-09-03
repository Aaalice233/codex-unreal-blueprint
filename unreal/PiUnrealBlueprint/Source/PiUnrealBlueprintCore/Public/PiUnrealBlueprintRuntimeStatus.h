#pragma once

#include "CoreMinimal.h"
#include "PiUnrealBlueprintProtocol.h"

namespace PiUnrealBlueprint
{
    struct PIUNREALBLUEPRINTCORE_API FRuntimeTransportStatus
    {
        EServiceState State = EServiceState::Stopped;
        bool bAvailable = false;
        FProtocolError Error;
    };

    class PIUNREALBLUEPRINTCORE_API FRuntimeStatusRegistry
    {
    public:
        static FRuntimeStatusRegistry& Get();

        void SetTransportStatus(EServiceState State, bool bAvailable, const FProtocolError& Error = FProtocolError());
        FRuntimeTransportStatus GetTransportStatus() const;

    private:
        mutable FCriticalSection Mutex;
        FRuntimeTransportStatus TransportStatus;
    };
}
