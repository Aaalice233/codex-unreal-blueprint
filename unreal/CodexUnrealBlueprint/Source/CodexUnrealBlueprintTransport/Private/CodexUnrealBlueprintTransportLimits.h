#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"

namespace CodexUnrealBlueprint
{
    struct FTransportLimits
    {
        static constexpr int32 MaxConnections = 16;
        static constexpr int32 MaxPendingRequestsPerConnection = 64;
        static constexpr int32 MaxOutgoingMessagesPerConnection = 256;
        static constexpr int32 MaxOutgoingBytesPerConnection = 16 * 1024 * 1024;
        static constexpr int32 MaxEmergencyFrameBytes = 64 * 1024;
        static constexpr double AuthenticationTimeoutSeconds = 5.0;
        static constexpr double IdleTimeoutSeconds = 15.0 * 60.0;
        static constexpr double SendTimeoutSeconds = 15.0;
    };

    class FBoundedTransportQueue
    {
    public:
        bool Enqueue(TArray<uint8>&& Frame);
        bool EnqueueEmergency(TArray<uint8>&& Frame);
        bool Dequeue(TArray<uint8>& OutFrame);
        bool IsEmpty() const;
        int32 Num() const;
        int32 NumBytes() const;

    private:
        mutable FCriticalSection Mutex;
        TQueue<TArray<uint8>, EQueueMode::Mpsc> Frames;
        int32 MessageCount = 0;
        int32 ByteCount = 0;
        bool bEmergencyQueued = false;
    };

    class FPendingRequestLimiter
    {
    public:
        bool TryAcquire();
        void Release();
        int32 Num() const;

    private:
        mutable FCriticalSection Mutex;
        int32 RequestCount = 0;
    };
}
