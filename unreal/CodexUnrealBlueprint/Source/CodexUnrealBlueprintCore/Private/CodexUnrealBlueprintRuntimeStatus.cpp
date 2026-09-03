#include "CodexUnrealBlueprintRuntimeStatus.h"

namespace CodexUnrealBlueprint
{
    FRuntimeStatusRegistry& FRuntimeStatusRegistry::Get()
    {
        static FRuntimeStatusRegistry Registry;
        return Registry;
    }

    void FRuntimeStatusRegistry::SetTransportStatus(
        const EServiceState State, const bool bAvailable, const FProtocolError& Error)
    {
        FScopeLock Lock(&Mutex);
        TransportStatus.State = State;
        TransportStatus.bAvailable = bAvailable;
        TransportStatus.Error = Error;
    }

    FRuntimeTransportStatus FRuntimeStatusRegistry::GetTransportStatus() const
    {
        FScopeLock Lock(&Mutex);
        return TransportStatus;
    }
}
