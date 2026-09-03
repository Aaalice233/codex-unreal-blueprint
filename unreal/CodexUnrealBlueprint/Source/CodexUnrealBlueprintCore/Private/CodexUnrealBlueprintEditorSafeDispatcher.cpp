#include "CodexUnrealBlueprintEditorSafeDispatcher.h"

namespace CodexUnrealBlueprint
{
    struct FEditorSafeDispatcher::FEntry
    {
        FWork Work;
        FWork OnCancelled;
    };

    FEditorSafeDispatcher& FEditorSafeDispatcher::Get()
    {
        static FEditorSafeDispatcher Dispatcher;
        return Dispatcher;
    }

    void FEditorSafeDispatcher::Start()
    {
        check(IsInGameThread());
        FScopeLock Lock(&Mutex);
        bAccepting = true;
    }

    bool FEditorSafeDispatcher::Enqueue(FWork Work, FWork OnCancelled)
    {
        if (!Work)
        {
            return false;
        }

        FScopeLock Lock(&Mutex);
        if (!bAccepting)
        {
            return false;
        }

        TSharedPtr<FEntry, ESPMode::ThreadSafe> Entry = MakeShared<FEntry, ESPMode::ThreadSafe>();
        Entry->Work = MoveTemp(Work);
        Entry->OnCancelled = MoveTemp(OnCancelled);
        Queue.Enqueue(MoveTemp(Entry));
        return true;
    }

    void FEditorSafeDispatcher::Tick()
    {
        check(IsInGameThread());

        // The Core ticker runs after GEngine::Tick. Destructive editor work such as package reload must
        // stay here instead of a generic GameThread task, which UE may execute inside a world tick group.
        constexpr int32 MaxDispatchesPerFrame = 32;
        for (int32 Index = 0; Index < MaxDispatchesPerFrame; ++Index)
        {
            TSharedPtr<FEntry, ESPMode::ThreadSafe> Entry;
            {
                FScopeLock Lock(&Mutex);
                if (!Queue.Dequeue(Entry))
                {
                    return;
                }
            }
            if (Entry.IsValid() && Entry->Work)
            {
                Entry->Work();
            }
        }
    }

    void FEditorSafeDispatcher::Shutdown()
    {
        check(IsInGameThread());

        TArray<TSharedPtr<FEntry, ESPMode::ThreadSafe>> Cancelled;
        {
            FScopeLock Lock(&Mutex);
            bAccepting = false;
            TSharedPtr<FEntry, ESPMode::ThreadSafe> Entry;
            while (Queue.Dequeue(Entry))
            {
                Cancelled.Add(MoveTemp(Entry));
            }
        }

        for (const TSharedPtr<FEntry, ESPMode::ThreadSafe>& Entry : Cancelled)
        {
            if (Entry.IsValid() && Entry->OnCancelled)
            {
                Entry->OnCancelled();
            }
        }
    }
}
