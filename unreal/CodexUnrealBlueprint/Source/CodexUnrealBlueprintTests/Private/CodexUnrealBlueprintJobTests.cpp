#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/ThreadSafeCounter.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "CodexUnrealBlueprintJobs.h"
#include "CodexUnrealBlueprintRequestJournal.h"
#include "CodexUnrealBlueprintTestFixture.h"

using namespace CodexUnrealBlueprint;
using namespace CodexUnrealBlueprintTests;

namespace
{
    bool PumpUntilTerminal(FJobManager& Manager, const TArray<FString>& JobIds, const double TimeoutSeconds)
    {
        const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
        while (FPlatformTime::Seconds() < Deadline)
        {
            Manager.Tick(FPlatformTime::Seconds());
            bool bAllTerminal = true;
            for (const FString& JobId : JobIds)
            {
                FJobSnapshot Snapshot;
                bAllTerminal = bAllTerminal && Manager.Get(JobId, Snapshot) && Snapshot.bTerminal;
            }
            if (bAllTerminal) return true;
            FPlatformProcess::Sleep(0.005f);
        }
        return false;
    }

    FString MakeJournalTestDirectory()
    {
        return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CodexUnrealBlueprintTests"),
            FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    TSharedPtr<FJsonObject> MakeParams(const FString& RequestId, const int32 Value)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("requestId"), RequestId);
        Params->SetNumberField(TEXT("value"), Value);
        return Params;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodexRequestJournalIdempotencyTest,
    "CodexUnrealBlueprint.Idempotency.CompletedResponseLossAndConflict", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexRequestJournalIdempotencyTest::RunTest(const FString& Parameters)
{
    const FString Directory = MakeJournalTestDirectory();
    FScopedDirectory DirectoryCleanup(Directory);
    FRequestJournal Journal(Directory, 2);
    FProtocolError Error;
    TestTrue(TEXT("journal initializes"), Journal.Initialize(Error));

    FRequestJournalRecord Accepted;
    TestEqual(TEXT("first payload is accepted"),
        Journal.Accept(TEXT("request-complete"), TEXT("blueprint.apply"), MakeParams(TEXT("request-complete"), 7),
            TEXT("job-complete"), Accepted, Error), ERequestAcceptResult::Accepted);
    FRequestJournalRecord Running;
    TestTrue(TEXT("accepted request becomes running"), Journal.MarkRunning(TEXT("request-complete"), Running, Error));
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("changed"), true);
    FRequestJournalRecord Terminal;
    TestTrue(TEXT("completed write is persisted before response"),
        Journal.MarkTerminal(TEXT("request-complete"), Result, TOptional<FProtocolError>(), Terminal, Error));

    FRequestJournal Restarted(Directory, 1);
    TestTrue(TEXT("restart loads terminal journal"), Restarted.Initialize(Error));
    FRequestJournalRecord Replay;
    TestEqual(TEXT("same request after response loss is replay-only"),
        Restarted.Accept(TEXT("request-complete"), TEXT("blueprint.apply"), MakeParams(TEXT("request-complete"), 7),
            TEXT("unused-new-job"), Replay, Error), ERequestAcceptResult::Replay);
    TestEqual(TEXT("replay keeps original job"), Replay.JobId, FString(TEXT("job-complete")));
    TestTrue(TEXT("replay keeps original result"), Replay.Result.IsValid() && Replay.Result->GetBoolField(TEXT("changed")));

    FRequestJournalRecord Conflict;
    TestEqual(TEXT("same id with changed payload conflicts"),
        Restarted.Accept(TEXT("request-complete"), TEXT("blueprint.apply"), MakeParams(TEXT("request-complete"), 8),
            TEXT("unused-conflict-job"), Conflict, Error), ERequestAcceptResult::Conflict);
    TestEqual(TEXT("conflict has stable code"), Error.Code, EErrorCode::RequestConflict);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodexRequestJournalConcurrentDuplicateTest,
    "CodexUnrealBlueprint.Idempotency.ConcurrentDuplicate", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexRequestJournalConcurrentDuplicateTest::RunTest(const FString& Parameters)
{
    const FString Directory = MakeJournalTestDirectory();
    FScopedDirectory DirectoryCleanup(Directory);
    FRequestJournal FirstJournal(Directory, 2);
    FRequestJournal SecondJournal(Directory, 2);
    FProtocolError Error;
    TestTrue(TEXT("first journal initializes"), FirstJournal.Initialize(Error));
    TestTrue(TEXT("second independent journal initializes"), SecondJournal.Initialize(Error));
    FRequestJournal* Journals[] = { &FirstJournal, &SecondJournal };

    TArray<TFuture<ERequestAcceptResult>> DuplicateFutures;
    for (int32 Index = 0; Index < 8; ++Index)
    {
        FRequestJournal* Journal = Journals[Index % 2];
        DuplicateFutures.Add(Async(EAsyncExecution::ThreadPool, [Journal, Index]()
        {
            FRequestJournalRecord Record;
            FProtocolError LocalError;
            return Journal->Accept(TEXT("request-concurrent"), TEXT("blueprint.apply"),
                MakeParams(TEXT("request-concurrent"), 10), FString::Printf(TEXT("job-%d"), Index), Record, LocalError);
        }));
    }
    int32 AcceptedCount = 0;
    int32 ReplayCount = 0;
    int32 FailedCount = 0;
    for (TFuture<ERequestAcceptResult>& Future : DuplicateFutures)
    {
        const ERequestAcceptResult Result = Future.Get();
        AcceptedCount += Result == ERequestAcceptResult::Accepted ? 1 : 0;
        ReplayCount += Result == ERequestAcceptResult::Replay ? 1 : 0;
        FailedCount += Result == ERequestAcceptResult::Failed ? 1 : 0;
    }
    TestEqual(TEXT("independent journal instances accept only once"), AcceptedCount, 1);
    TestEqual(TEXT("same payload is replay-only after the winner"), ReplayCount, 7);
    TestEqual(TEXT("cross-instance locking has no hidden I/O failures"), FailedCount, 0);

    TArray<TFuture<ERequestAcceptResult>> ConflictFutures;
    for (int32 Index = 0; Index < 8; ++Index)
    {
        FRequestJournal* Journal = Journals[Index % 2];
        ConflictFutures.Add(Async(EAsyncExecution::ThreadPool, [Journal, Index]()
        {
            FRequestJournalRecord Record;
            FProtocolError LocalError;
            return Journal->Accept(TEXT("request-conflict-race"), TEXT("blueprint.apply"),
                MakeParams(TEXT("request-conflict-race"), Index % 2), FString::Printf(TEXT("conflict-job-%d"), Index),
                Record, LocalError);
        }));
    }
    AcceptedCount = 0;
    ReplayCount = 0;
    int32 ConflictCount = 0;
    FailedCount = 0;
    for (TFuture<ERequestAcceptResult>& Future : ConflictFutures)
    {
        const ERequestAcceptResult Result = Future.Get();
        AcceptedCount += Result == ERequestAcceptResult::Accepted ? 1 : 0;
        ReplayCount += Result == ERequestAcceptResult::Replay ? 1 : 0;
        ConflictCount += Result == ERequestAcceptResult::Conflict ? 1 : 0;
        FailedCount += Result == ERequestAcceptResult::Failed ? 1 : 0;
    }
    TestEqual(TEXT("conflicting payload race has one winner"), AcceptedCount, 1);
    TestEqual(TEXT("winner payload replays in its other attempts"), ReplayCount, 3);
    TestEqual(TEXT("losing payload always conflicts"), ConflictCount, 4);
    TestEqual(TEXT("conflict race has no hidden I/O failures"), FailedCount, 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodexRequestJournalRestartRecoveryTest,
    "CodexUnrealBlueprint.Idempotency.RestartMarksInflightInterrupted", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexRequestJournalRestartRecoveryTest::RunTest(const FString& Parameters)
{
    const FString Directory = MakeJournalTestDirectory();
    FScopedDirectory DirectoryCleanup(Directory);
    FProtocolError Error;
    {
        FRequestJournal Journal(Directory);
        TestTrue(TEXT("journal initializes"), Journal.Initialize(Error));
        FRequestJournalRecord Record;
        TestEqual(TEXT("write is accepted"), Journal.Accept(TEXT("request-inflight"), TEXT("blueprint.apply"),
            MakeParams(TEXT("request-inflight"), 2), TEXT("job-inflight"), Record, Error), ERequestAcceptResult::Accepted);
        TestTrue(TEXT("write is running"), Journal.MarkRunning(TEXT("request-inflight"), Record, Error));
    }
    FRequestJournal Restarted(Directory);
    TestTrue(TEXT("restart recovers journal"), Restarted.Initialize(Error));
    FRequestJournalRecord Recovered;
    TestTrue(TEXT("interrupted request remains queryable"), Restarted.Query(TEXT("request-inflight"), Recovered, Error));
    TestEqual(TEXT("interrupted request is terminal"), Recovered.State, ERequestJournalState::Terminal);
    TestTrue(TEXT("interruption is explicit"), Recovered.bInterrupted);
    TestTrue(TEXT("recovery is required"), Recovered.bRecoveryRequired);
    TestTrue(TEXT("interruption has an error"), Recovered.Error.IsSet());
    if (Recovered.Error.IsSet()) TestEqual(TEXT("interruption code"), Recovered.Error.GetValue().Code, EErrorCode::RequestInterrupted);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodexRequestJournalCorruptionTest,
    "CodexUnrealBlueprint.Idempotency.CorruptJournalFailsExplicitly", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexRequestJournalCorruptionTest::RunTest(const FString& Parameters)
{
    const FString Directory = MakeJournalTestDirectory();
    FScopedDirectory DirectoryCleanup(Directory);
    IFileManager::Get().MakeDirectory(*Directory, true);
    TestTrue(TEXT("corrupt fixture is written"), FFileHelper::SaveStringToFile(TEXT("{not-json"),
        *FPaths::Combine(Directory, TEXT("corrupt.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
    FRequestJournal Journal(Directory);
    FProtocolError Error;
    TestFalse(TEXT("corrupt journal blocks initialization"), Journal.Initialize(Error));
    TestEqual(TEXT("corruption is explicit"), Error.Code, EErrorCode::JournalCorrupt);
    FRequestJournalStatus Status;
    TestFalse(TEXT("status does not hide corruption"), Journal.GetStatus(Status, Error));
    TestFalse(TEXT("status is unhealthy"), Status.bHealthy);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodexJobReadConcurrencyTest,
    "CodexUnrealBlueprint.Jobs.ConcurrentReads", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexJobReadConcurrencyTest::RunTest(const FString& Parameters)
{
    FJobManager& Manager = FJobManager::Get();
    FThreadSafeCounter Active;
    FThreadSafeCounter Maximum;
    auto ReadWork = [&Active, &Maximum](FJobExecutionContext& Context, TSharedPtr<FJsonObject>& Result, FProtocolError& Error)
    {
        const int32 Current = Active.Increment();
        while (Maximum.GetValue() < Current) Maximum.Increment();
        Context.ReportProgress(1, 1, TEXT("read"));
        FPlatformProcess::Sleep(0.05f);
        Active.Decrement();
        Result = MakeShared<FJsonObject>();
        return true;
    };
    const FString First = Manager.Start(EJobAccess::Read, FString(), ReadWork);
    const FString Second = Manager.Start(EJobAccess::Read, FString(), ReadWork);
    TestTrue(TEXT("read jobs complete"), PumpUntilTerminal(Manager, { First, Second }, 2.0));
    TestTrue(TEXT("reads overlap"), Maximum.GetValue() >= 2);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodexJobSingleWriterTest,
    "CodexUnrealBlueprint.Jobs.GlobalSingleWriter", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexJobSingleWriterTest::RunTest(const FString& Parameters)
{
    FJobManager& Manager = FJobManager::Get();
    FThreadSafeCounter Active;
    FThreadSafeCounter Overlap;
    auto WriteWork = [&Active, &Overlap](FJobExecutionContext& Context, TSharedPtr<FJsonObject>& Result, FProtocolError& Error)
    {
        if (Active.Increment() > 1) Overlap.Increment();
        Context.EnterPhase(EJobPhase::Modify, true, TEXT("write"));
        FPlatformProcess::Sleep(0.04f);
        Active.Decrement();
        Result = MakeShared<FJsonObject>();
        return true;
    };
    const FString First = Manager.Start(EJobAccess::Write, TEXT("writer-1"), WriteWork);
    const FString Second = Manager.Start(EJobAccess::Write, TEXT("writer-2"), WriteWork);
    TestTrue(TEXT("write jobs complete"), PumpUntilTerminal(Manager, { First, Second }, 2.0));
    TestEqual(TEXT("writers never overlap"), Overlap.GetValue(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodexJobSafeCancellationTest,
    "CodexUnrealBlueprint.Jobs.SafeCancellation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexJobSafeCancellationTest::RunTest(const FString& Parameters)
{
    FJobManager& Manager = FJobManager::Get();
    FThreadSafeCounter EnteredSafe;
    const FString JobId = Manager.Start(EJobAccess::Write,
        FString(TEXT("cancel-safe-")) + FGuid::NewGuid().ToString(EGuidFormats::Digits),
        [&EnteredSafe](FJobExecutionContext& Context, TSharedPtr<FJsonObject>& Result, FProtocolError& Error)
        {
            Context.EnterPhase(EJobPhase::Modify, true, TEXT("safe modification"));
            EnteredSafe.Increment();
            while (!Context.IsCancellationRequested()) FPlatformProcess::Sleep(0.002f);
            return false;
        });
    const double Deadline = FPlatformTime::Seconds() + 1.0;
    while (EnteredSafe.GetValue() == 0 && FPlatformTime::Seconds() < Deadline)
    {
        Manager.Tick(FPlatformTime::Seconds());
        FPlatformProcess::Sleep(0.002f);
    }
    FJobSnapshot Cancelled;
    FProtocolError Error;
    TestTrue(TEXT("safe cancel is accepted"), Manager.Cancel(JobId, Cancelled, Error));
    TestTrue(TEXT("safe cancel reaches terminal"), PumpUntilTerminal(Manager, { JobId }, 2.0));
    FJobSnapshot Final;
    Manager.Get(JobId, Final);
    TestEqual(TEXT("safe cancel terminal phase"), Final.Phase, EJobPhase::Cancelled);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodexJobCancellationTest,
    "CodexUnrealBlueprint.Jobs.UnsafeCancellationPreservesFailure", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexJobCancellationTest::RunTest(const FString& Parameters)
{
    FJobManager& Manager = FJobManager::Get();
    FThreadSafeCounter EnteredUnsafe;
    FThreadSafeCounter ReleaseUnsafe;
    const FString JobId = Manager.Start(EJobAccess::Write,
        FString(TEXT("cancel-unsafe-")) + FGuid::NewGuid().ToString(EGuidFormats::Digits),
        [&EnteredUnsafe, &ReleaseUnsafe](FJobExecutionContext& Context, TSharedPtr<FJsonObject>& Result, FProtocolError& Error)
        {
            Context.EnterPhase(EJobPhase::Save, false, TEXT("native save"));
            EnteredUnsafe.Increment();
            while (ReleaseUnsafe.GetValue() == 0) FPlatformProcess::Sleep(0.002f);
            Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("success"), false);
            Result->SetBoolField(TEXT("partial"), true);
            Result->SetBoolField(TEXT("stateUnknown"), true);
            Error = FProtocolError::Make(EErrorCode::VerificationFailed,
                TEXT("Saved assets could not be verified."), TEXT("FCodexJobCancellationTest"));
            return false;
        });
    const double Deadline = FPlatformTime::Seconds() + 1.0;
    while (EnteredUnsafe.GetValue() == 0 && FPlatformTime::Seconds() < Deadline)
    {
        Manager.Tick(FPlatformTime::Seconds());
        FPlatformProcess::Sleep(0.002f);
    }
    FJobSnapshot Cancelled;
    FProtocolError Error;
    TestTrue(TEXT("unsafe cancel returns the current snapshot"), Manager.Cancel(JobId, Cancelled, Error));
    TestTrue(TEXT("unsafe cancel is explicitly deferred"), Cancelled.bCancellationDeferred);
    TestEqual(TEXT("deferred disposition is serialized"),
        Cancelled.ToJson()->GetStringField(TEXT("cancellationDisposition")), FString(TEXT("deferred")));
    TestFalse(TEXT("unsafe cancel does not request cancellation yet"), Cancelled.bCancellationRequested);
    TestFalse(TEXT("unsafe running job is not falsely terminal"), Cancelled.bTerminal);

    ReleaseUnsafe.Increment();
    TestTrue(TEXT("failed save reaches terminal"), PumpUntilTerminal(Manager, { JobId }, 2.0));
    FJobSnapshot Final;
    Manager.Get(JobId, Final);
    TestEqual(TEXT("saved failure is not overwritten as cancelled"), Final.Phase, EJobPhase::Failed);
    TestTrue(TEXT("partial result is retained"), Final.Result.IsValid() && Final.Result->GetBoolField(TEXT("partial")));
    TestTrue(TEXT("unknown state is retained"), Final.Result.IsValid() && Final.Result->GetBoolField(TEXT("stateUnknown")));
    TestTrue(TEXT("original failure is retained"), Final.Error.IsSet() && Final.Error.GetValue().Code == EErrorCode::VerificationFailed);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodexJobQueueLimitTest,
    "CodexUnrealBlueprint.Jobs.QueueLimit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexJobQueueLimitTest::RunTest(const FString& Parameters)
{
    FJobManager& Manager = FJobManager::Get();
    TestEqual(TEXT("queue starts empty"), Manager.GetStatus().QueuedCount, 0);
    TArray<FString> JobIds;
    for (int32 Index = 0; Index < FJobManager::MaxQueuedJobs; ++Index)
    {
        FProtocolError StartError;
        const FString JobId = Manager.Start(EJobAccess::Read, FString(),
            [](FJobExecutionContext&, TSharedPtr<FJsonObject>& Result, FProtocolError&)
            {
                Result = MakeShared<FJsonObject>();
                return true;
            }, &StartError);
        TestFalse(TEXT("jobs inside the queue limit are accepted"), JobId.IsEmpty());
        JobIds.Add(JobId);
    }
    FProtocolError OverflowError;
    const FString Overflow = Manager.Start(EJobAccess::Read, FString(),
        [](FJobExecutionContext&, TSharedPtr<FJsonObject>&, FProtocolError&) { return true; }, &OverflowError);
    TestTrue(TEXT("job above the queue limit is rejected"), Overflow.IsEmpty());
    TestEqual(TEXT("queue rejection has a stable code"), OverflowError.Code, EErrorCode::JobQueueFull);
    for (const FString& JobId : JobIds)
    {
        FJobSnapshot Snapshot;
        FProtocolError CancelError;
        Manager.Cancel(JobId, Snapshot, CancelError);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodexWriteLeaseHeartbeatTest,
    "CodexUnrealBlueprint.Jobs.WriteLeaseHeartbeatExpiry", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexWriteLeaseHeartbeatTest::RunTest(const FString& Parameters)
{
    FReadWriteLease Lease(1.0);
    TestTrue(TEXT("writer acquires empty lease"), Lease.TryAcquireWrite(TEXT("writer"), 10.0));
    TestTrue(TEXT("heartbeat refreshes owner"), Lease.HeartbeatWrite(TEXT("writer"), 10.75));
    FString Expired;
    TestFalse(TEXT("fresh heartbeat does not expire"), Lease.IsWriteExpired(11.5, Expired));
    TestTrue(TEXT("stale heartbeat is reported"), Lease.IsWriteExpired(11.76, Expired));
    TestEqual(TEXT("expired owner is reported"), Expired, FString(TEXT("writer")));
    TestFalse(TEXT("expired writer still owns the lease"), Lease.TryAcquireWrite(TEXT("next"), 12.0));
    Lease.ReleaseWrite(TEXT("writer"));
    TestTrue(TEXT("new writer acquires only after safe release"), Lease.TryAcquireWrite(TEXT("next"), 12.0));
    TestFalse(TEXT("readers are blocked by writer"), Lease.TryAcquireRead(TEXT("reader")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodexTimedOutWriterRecoveryTest,
    "CodexUnrealBlueprint.Jobs.TimedOutWriterRecovery", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexTimedOutWriterRecoveryTest::RunTest(const FString& Parameters)
{
    FJobManager& Manager = FJobManager::Get();
    FThreadSafeCounter FirstEntered;
    FThreadSafeCounter ReleaseFirst;
    FThreadSafeCounter SecondEntered;
    const FString First = Manager.Start(EJobAccess::Write, TEXT("timeout-writer-1"),
        [&FirstEntered, &ReleaseFirst](FJobExecutionContext& Context, TSharedPtr<FJsonObject>& Result, FProtocolError& Error)
        {
            FirstEntered.Increment();
            while (ReleaseFirst.GetValue() == 0) FPlatformProcess::Sleep(0.002f);
            Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("success"), true);
            Result->SetBoolField(TEXT("partial"), false);
            Result->SetBoolField(TEXT("stateUnknown"), false);
            return true;
        });

    const double StartDeadline = FPlatformTime::Seconds() + 1.0;
    while (FirstEntered.GetValue() == 0 && FPlatformTime::Seconds() < StartDeadline)
    {
        Manager.Tick(FPlatformTime::Seconds());
        FPlatformProcess::Sleep(0.002f);
    }
    TestEqual(TEXT("first writer started"), FirstEntered.GetValue(), 1);

    Manager.Tick(FPlatformTime::Seconds() + 16.0);
    FJobSnapshot Recovering;
    TestTrue(TEXT("timed out writer remains queryable"), Manager.Get(First, Recovering));
    TestEqual(TEXT("timed out writer enters stopping"), Recovering.Phase, EJobPhase::Stopping);
    TestTrue(TEXT("stopping requests cooperative cancellation"), Recovering.bCancellationRequested);
    TestFalse(TEXT("stopping is not terminal while worker still runs"), Recovering.bTerminal);

    const FString Second = Manager.Start(EJobAccess::Write, TEXT("timeout-writer-2"),
        [&SecondEntered](FJobExecutionContext& Context, TSharedPtr<FJsonObject>& Result, FProtocolError& Error)
        {
            SecondEntered.Increment();
            Result = MakeShared<FJsonObject>();
            return true;
        });
    Manager.Tick(FPlatformTime::Seconds() + 17.0);
    FPlatformProcess::Sleep(0.02f);
    TestEqual(TEXT("second writer waits for old worker to stop"), SecondEntered.GetValue(), 0);

    ReleaseFirst.Increment();
    TestTrue(TEXT("both writers reach terminal state"), PumpUntilTerminal(Manager, { First, Second }, 2.0));
    TestEqual(TEXT("second writer starts after recovery release"), SecondEntered.GetValue(), 1);
    FJobSnapshot FirstFinal;
    Manager.Get(First, FirstFinal);
    TestEqual(TEXT("timed out writer fails explicitly"), FirstFinal.Phase, EJobPhase::Failed);
    TestTrue(TEXT("timed out writer has a stable error"),
        FirstFinal.Error.IsSet() && FirstFinal.Error.GetValue().Code == EErrorCode::WriteLeaseExpired);
    TestTrue(TEXT("timeout result cannot still claim success"),
        FirstFinal.Result.IsValid() && !FirstFinal.Result->GetBoolField(TEXT("success")));
    TestTrue(TEXT("timeout result requires state recovery"), FirstFinal.Result.IsValid()
        && FirstFinal.Result->GetBoolField(TEXT("partial"))
        && FirstFinal.Result->GetBoolField(TEXT("stateUnknown")));
    return true;
}

#endif
