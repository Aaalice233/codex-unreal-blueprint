#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"

namespace CodexUnrealBlueprint
{
    class CODEXUNREALBLUEPRINTCORE_API FEditorSafeDispatcher
    {
    public:
        using FWork = TFunction<void()>;

        static FEditorSafeDispatcher& Get();

        void Start();
        bool Enqueue(FWork Work, FWork OnCancelled = FWork());
        void Tick();
        void Shutdown();

    private:
        struct FEntry;

        FCriticalSection Mutex;
        TQueue<TSharedPtr<FEntry, ESPMode::ThreadSafe>, EQueueMode::Mpsc> Queue;
        bool bAccepting = false;
    };
}
