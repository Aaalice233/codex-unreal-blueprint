#include "CodexUnrealBlueprintJobs.h"

#include "CodexUnrealBlueprintRequestJournal.h"
#include "Async/Async.h"
#include "Async/TaskGraphInterfaces.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/Guid.h"

namespace CodexUnrealBlueprint
{
    namespace
    {
        bool IsTerminal(const EJobPhase Phase)
        {
            return Phase == EJobPhase::Succeeded || Phase == EJobPhase::Failed || Phase == EJobPhase::Cancelled;
        }

        FString AccessToString(const EJobAccess Access)
        {
            return Access == EJobAccess::Write ? TEXT("write") : TEXT("read");
        }

        FJobSnapshot SnapshotFromJournal(const FRequestJournalRecord& Record)
        {
            FJobSnapshot Snapshot;
            Snapshot.JobId = Record.JobId;
            Snapshot.RequestId = Record.RequestId;
            Snapshot.Access = EJobAccess::Write;
            Snapshot.CreatedAt = Record.AcceptedAt;
            Snapshot.UpdatedAt = Record.UpdatedAt;
            Snapshot.Result = Record.Result;
            Snapshot.Error = Record.Error;
            Snapshot.bTerminal = Record.State == ERequestJournalState::Terminal;
            Snapshot.Phase = Snapshot.bTerminal
                ? Record.TerminalPhase
                : (Record.State == ERequestJournalState::Running ? EJobPhase::Modify : EJobPhase::Queued);
            Snapshot.bCancellationRequested = Snapshot.Phase == EJobPhase::Cancelled;
            Snapshot.bCancellationSafe = Record.State != ERequestJournalState::Running;
            return Snapshot;
        }
    }

    TSharedRef<FJsonObject> FJobProgress::ToJson() const
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("jobId"), JobId);
        Json->SetStringField(TEXT("phase"), LexToString(Phase));
        Json->SetNumberField(TEXT("completed"), Completed);
        Json->SetNumberField(TEXT("total"), Total);
        Json->SetStringField(TEXT("message"), Message);
        if (!AssetPath.IsEmpty()) Json->SetStringField(TEXT("assetPath"), AssetPath);
        Json->SetStringField(TEXT("timestamp"), Timestamp.ToIso8601());
        return Json;
    }

    TSharedRef<FJsonObject> FJobManagerStatus::ToJson() const
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetNumberField(TEXT("jobCount"), JobCount);
        Json->SetNumberField(TEXT("queued"), QueuedCount);
        Json->SetNumberField(TEXT("running"), RunningCount);
        Json->SetNumberField(TEXT("terminal"), TerminalCount);
        Json->SetNumberField(TEXT("queuedReads"), QueuedReadCount);
        Json->SetNumberField(TEXT("queuedWrites"), QueuedWriteCount);
        return Json;
    }

    TSharedRef<FJsonObject> FJobSnapshot::ToJson() const
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("jobId"), JobId);
        Json->SetStringField(TEXT("requestId"), RequestId);
        Json->SetStringField(TEXT("access"), AccessToString(Access));
        Json->SetStringField(TEXT("phase"), LexToString(Phase));
        Json->SetBoolField(TEXT("terminal"), bTerminal);
        Json->SetBoolField(TEXT("cancellationRequested"), bCancellationRequested);
        Json->SetBoolField(TEXT("cancellationDeferred"), bCancellationDeferred);
        Json->SetStringField(TEXT("cancellationDisposition"),
            bCancellationDeferred ? TEXT("deferred") : (bCancellationRequested ? TEXT("requested") : TEXT("none")));
        Json->SetBoolField(TEXT("cancellationSafe"), bCancellationSafe);
        Json->SetStringField(TEXT("createdAt"), CreatedAt.ToIso8601());
        Json->SetStringField(TEXT("updatedAt"), UpdatedAt.ToIso8601());
        if (Result.IsValid()) Json->SetObjectField(TEXT("result"), Result.ToSharedRef());
        if (Error.IsSet()) Json->SetObjectField(TEXT("error"), Error.GetValue().ToJson());
        TArray<TSharedPtr<FJsonValue>> Items;
        for (const FJobProgress& Item : Progress) Items.Add(MakeShared<FJsonValueObject>(Item.ToJson()));
        Json->SetArrayField(TEXT("progress"), Items);
        return Json;
    }

    FReadWriteLease::FReadWriteLease(const double InWriteTimeoutSeconds)
        : WriteTimeoutSeconds(FMath::Max(0.001, InWriteTimeoutSeconds))
    {
    }

    bool FReadWriteLease::TryAcquireRead(const FString& OwnerId)
    {
        FScopeLock Lock(&Mutex);
        if (OwnerId.IsEmpty() || !WriterId.IsEmpty()) return false;
        Readers.Add(OwnerId);
        return true;
    }

    void FReadWriteLease::ReleaseRead(const FString& OwnerId)
    {
        FScopeLock Lock(&Mutex);
        Readers.Remove(OwnerId);
    }

    bool FReadWriteLease::TryAcquireWrite(const FString& OwnerId, const double NowSeconds)
    {
        FScopeLock Lock(&Mutex);
        if (OwnerId.IsEmpty() || !WriterId.IsEmpty() || Readers.Num() > 0) return false;
        WriterId = OwnerId;
        WriterHeartbeatSeconds = NowSeconds;
        return true;
    }

    bool FReadWriteLease::HeartbeatWrite(const FString& OwnerId, const double NowSeconds)
    {
        FScopeLock Lock(&Mutex);
        if (WriterId != OwnerId) return false;
        WriterHeartbeatSeconds = NowSeconds;
        return true;
    }

    void FReadWriteLease::ReleaseWrite(const FString& OwnerId)
    {
        FScopeLock Lock(&Mutex);
        if (WriterId == OwnerId)
        {
            WriterId.Reset();
            WriterHeartbeatSeconds = 0.0;
        }
    }

    bool FReadWriteLease::IsWriteExpired(const double NowSeconds, FString& OutExpiredOwnerId) const
    {
        FScopeLock Lock(&Mutex);
        if (WriterId.IsEmpty() || NowSeconds - WriterHeartbeatSeconds <= WriteTimeoutSeconds) return false;
        OutExpiredOwnerId = WriterId;
        return true;
    }

    int32 FReadWriteLease::GetReaderCount() const
    {
        FScopeLock Lock(&Mutex);
        return Readers.Num();
    }

    FString FReadWriteLease::GetWriterId() const
    {
        FScopeLock Lock(&Mutex);
        return WriterId;
    }

    struct FJobManager::FImpl
    {
        struct FRecord
        {
            FJobSnapshot Snapshot;
            FJobWork Work;
            bool bRunning = false;
            bool bLeaseTimedOut = false;
            bool bJournaled = false;
        };

        struct FWaiter
        {
            FString JobId;
            double DeadlineSeconds = 0.0;
            FJobWaitCallback Callback;
        };

        mutable FCriticalSection Mutex;
        TMap<FString, TSharedPtr<FRecord, ESPMode::ThreadSafe>> Jobs;
        TMap<FString, FString> RequestJobs;
        TArray<FString> Queue;
        TArray<FWaiter> Waiters;
        FReadWriteLease Lease;
        FOnJobProgress ProgressDelegate;
        int32 ActiveWorkerCount = 0;
        bool bShuttingDown = false;
    };

    FJobExecutionContext::FJobExecutionContext(FJobManager& InManager, const FString& InJobId)
        : Manager(InManager), JobId(InJobId)
    {
    }

    bool FJobExecutionContext::EnterPhase(const EJobPhase Phase, const bool bCancellationSafe, const FString& Message)
    {
        return Manager.EnterPhase(JobId, Phase, bCancellationSafe, Message);
    }

    void FJobExecutionContext::ReportProgress(const int32 Completed, const int32 Total, const FString& Message, const FString& AssetPath)
    {
        Manager.ReportProgress(JobId, Completed, Total, Message, AssetPath);
    }

    void FJobExecutionContext::Heartbeat() { Manager.Heartbeat(JobId); }
    bool FJobExecutionContext::IsCancellationRequested() const { return Manager.IsCancellationRequested(JobId); }
    FString FJobExecutionContext::GetJobId() const { return JobId; }

    FJobManager& FJobManager::Get()
    {
        static FJobManager Manager;
        return Manager;
    }

    FJobManager::FJobManager() : Impl(MakeUnique<FImpl>()) {}
    FJobManager::~FJobManager() { Shutdown(); }

    FString FJobManager::Start(const EJobAccess Access, const FString& RequestId, FJobWork Work, FProtocolError* OutError)
    {
        if (OutError != nullptr) *OutError = FProtocolError();
        if (!Work || (Access == EJobAccess::Write && RequestId.TrimStartAndEnd().IsEmpty()))
        {
            if (OutError != nullptr) *OutError = FProtocolError::Make(EErrorCode::InvalidArgument,
                TEXT("Job work is required and write jobs require a non-empty requestId."), TEXT("FJobManager::Start"));
            return FString();
        }
        FScopeLock Lock(&Impl->Mutex);
        if (Impl->bShuttingDown)
        {
            if (OutError != nullptr) *OutError = FProtocolError::Make(EErrorCode::InternalError,
                TEXT("Job manager is shutting down."), TEXT("FJobManager::Start"));
            return FString();
        }
        if (!RequestId.IsEmpty())
        {
            if (const FString* Existing = Impl->RequestJobs.Find(RequestId)) return *Existing;
        }
        if (Impl->Queue.Num() >= MaxQueuedJobs)
        {
            if (OutError != nullptr) *OutError = FProtocolError::Make(EErrorCode::JobQueueFull,
                FString::Printf(TEXT("The global job queue limit of %d has been reached."), MaxQueuedJobs),
                TEXT("FJobManager::Start"));
            return FString();
        }
        const FString JobId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens).ToLower();
        TSharedPtr<FImpl::FRecord, ESPMode::ThreadSafe> Record = MakeShared<FImpl::FRecord, ESPMode::ThreadSafe>();
        Record->Snapshot.JobId = JobId;
        Record->Snapshot.RequestId = RequestId;
        Record->Snapshot.Access = Access;
        Record->Snapshot.CreatedAt = FDateTime::UtcNow();
        Record->Snapshot.UpdatedAt = Record->Snapshot.CreatedAt;
        Record->Work = MoveTemp(Work);
        Impl->Jobs.Add(JobId, Record);
        if (!RequestId.IsEmpty()) Impl->RequestJobs.Add(RequestId, JobId);
        Impl->Queue.Add(JobId);
        return JobId;
    }

    bool FJobManager::StartWrite(
        const FString& Method,
        const FString& RequestId,
        const TSharedPtr<FJsonObject>& Params,
        FJobWork Work,
        FJobSnapshot& OutSnapshot,
        FProtocolError& OutError,
        bool& bOutReplay)
    {
        bOutReplay = false;
        if (!Work || RequestId.TrimStartAndEnd().IsEmpty())
        {
            OutError = FProtocolError::Make(EErrorCode::RequestIdRequired,
                TEXT("Write requests require a non-empty requestId."), TEXT("FJobManager::StartWrite"));
            return false;
        }

        const FString ProposedJobId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens).ToLower();
        FRequestJournalRecord JournalRecord;
        const ERequestAcceptResult AcceptResult = FRequestJournal::Get().Accept(
            RequestId, Method, Params, ProposedJobId, JournalRecord, OutError);
        if (AcceptResult == ERequestAcceptResult::Conflict || AcceptResult == ERequestAcceptResult::Failed) return false;
        if (AcceptResult == ERequestAcceptResult::Replay)
        {
            bOutReplay = true;
            if (Get(JournalRecord.JobId, OutSnapshot)) return true;
            OutSnapshot = SnapshotFromJournal(JournalRecord);
            return true;
        }

        FScopeLock Lock(&Impl->Mutex);
        if (Impl->bShuttingDown)
        {
            OutError = FProtocolError::Make(EErrorCode::InternalError,
                TEXT("Job manager is shutting down; the accepted write will not run."), TEXT("FJobManager::StartWrite"));
            FRequestJournalRecord TerminalRecord;
            FProtocolError JournalError;
            if (!FRequestJournal::Get().MarkTerminal(RequestId, nullptr, OutError, EJobPhase::Failed, TerminalRecord, JournalError))
            {
                OutError = JournalError;
            }
            return false;
        }
        if (Impl->Queue.Num() >= MaxQueuedJobs)
        {
            OutError = FProtocolError::Make(EErrorCode::JobQueueFull,
                FString::Printf(TEXT("The global job queue limit of %d has been reached."), MaxQueuedJobs),
                TEXT("FJobManager::StartWrite"));
            FRequestJournalRecord TerminalRecord;
            FProtocolError JournalError;
            if (!FRequestJournal::Get().MarkTerminal(
                RequestId, nullptr, OutError, EJobPhase::Failed, TerminalRecord, JournalError))
            {
                OutError = JournalError;
            }
            return false;
        }
        TSharedPtr<FImpl::FRecord, ESPMode::ThreadSafe> Record = MakeShared<FImpl::FRecord, ESPMode::ThreadSafe>();
        Record->Snapshot = SnapshotFromJournal(JournalRecord);
        Record->Work = MoveTemp(Work);
        Record->bJournaled = true;
        Impl->Jobs.Add(JournalRecord.JobId, Record);
        Impl->RequestJobs.Add(RequestId, JournalRecord.JobId);
        Impl->Queue.Add(JournalRecord.JobId);
        OutSnapshot = Record->Snapshot;
        OutError = FProtocolError();
        return true;
    }

    bool FJobManager::Get(const FString& JobId, FJobSnapshot& OutSnapshot) const
    {
        FScopeLock Lock(&Impl->Mutex);
        const TSharedPtr<FImpl::FRecord, ESPMode::ThreadSafe>* Record = Impl->Jobs.Find(JobId);
        if (Record == nullptr) return false;
        OutSnapshot = (*Record)->Snapshot;
        return true;
    }

    bool FJobManager::FindByRequestId(const FString& RequestId, FJobSnapshot& OutSnapshot) const
    {
        FString JobId;
        {
            FScopeLock Lock(&Impl->Mutex);
            const FString* Found = Impl->RequestJobs.Find(RequestId);
            if (Found == nullptr) return false;
            JobId = *Found;
        }
        return Get(JobId, OutSnapshot);
    }

    void FJobManager::GetSnapshots(TArray<FJobSnapshot>& OutSnapshots) const
    {
        FScopeLock Lock(&Impl->Mutex);
        OutSnapshots.Reset(Impl->Jobs.Num());
        for (const TPair<FString, TSharedPtr<FImpl::FRecord, ESPMode::ThreadSafe>>& Pair : Impl->Jobs)
        {
            OutSnapshots.Add(Pair.Value->Snapshot);
        }
    }

    FJobManagerStatus FJobManager::GetStatus() const
    {
        FScopeLock Lock(&Impl->Mutex);
        FJobManagerStatus Status;
        Status.JobCount = Impl->Jobs.Num();
        for (const TPair<FString, TSharedPtr<FImpl::FRecord, ESPMode::ThreadSafe>>& Pair : Impl->Jobs)
        {
            const FImpl::FRecord& Record = *Pair.Value;
            if (Record.Snapshot.bTerminal)
            {
                ++Status.TerminalCount;
            }
            else if (Record.bRunning)
            {
                ++Status.RunningCount;
            }
            else
            {
                ++Status.QueuedCount;
                if (Record.Snapshot.Access == EJobAccess::Write) ++Status.QueuedWriteCount;
                else ++Status.QueuedReadCount;
            }
        }
        return Status;
    }

    bool FJobManager::Cancel(const FString& JobId, FJobSnapshot& OutSnapshot, FProtocolError& OutError)
    {
        FScopeLock Lock(&Impl->Mutex);
        const TSharedPtr<FImpl::FRecord, ESPMode::ThreadSafe>* Found = Impl->Jobs.Find(JobId);
        if (Found == nullptr)
        {
            OutError = FProtocolError::Make(EErrorCode::InvalidRequest, TEXT("Unknown jobId."), TEXT("FJobManager::Cancel"));
            return false;
        }
        FImpl::FRecord& Record = *Found->Get();
        if (!Record.Snapshot.bTerminal)
        {
            Record.Snapshot.UpdatedAt = FDateTime::UtcNow();
            if (Record.bRunning && !Record.Snapshot.bCancellationSafe)
            {
                // 不可中断阶段只登记延后请求，避免把已保存的失败结果伪装成取消成功。
                Record.Snapshot.bCancellationDeferred = true;
                OutSnapshot = Record.Snapshot;
                OutError = FProtocolError();
                return true;
            }
            if (!Record.bRunning && Record.bJournaled)
            {
                FRequestJournalRecord JournalRecord;
                if (!FRequestJournal::Get().MarkTerminal(
                    Record.Snapshot.RequestId, nullptr, TOptional<FProtocolError>(), EJobPhase::Cancelled, JournalRecord, OutError))
                {
                    return false;
                }
            }
            Record.Snapshot.bCancellationRequested = true;
            Record.Snapshot.bCancellationDeferred = false;
            if (!Record.bRunning)
            {
                Record.Snapshot.Phase = EJobPhase::Cancelled;
                Record.Snapshot.bTerminal = true;
                Record.Snapshot.bCancellationSafe = true;
                Impl->Queue.Remove(JobId);
            }
        }
        OutSnapshot = Record.Snapshot;
        OutError = FProtocolError();
        return true;
    }

    void FJobManager::Wait(const FString& JobId, const double TimeoutSeconds, FJobWaitCallback Callback)
    {
        FJobSnapshot Snapshot;
        if (!Get(JobId, Snapshot))
        {
            Snapshot.JobId = JobId;
            Snapshot.Phase = EJobPhase::Failed;
            Snapshot.bTerminal = true;
            Snapshot.Error = FProtocolError::Make(EErrorCode::InvalidRequest, TEXT("Unknown jobId."), TEXT("FJobManager::Wait"));
            Callback(Snapshot);
            return;
        }
        if (Snapshot.bTerminal || TimeoutSeconds <= 0.0)
        {
            Callback(Snapshot);
            return;
        }
        FScopeLock Lock(&Impl->Mutex);
        FImpl::FWaiter Waiter;
        Waiter.JobId = JobId;
        Waiter.DeadlineSeconds = FPlatformTime::Seconds() + FMath::Min(TimeoutSeconds, 600.0);
        Waiter.Callback = MoveTemp(Callback);
        Impl->Waiters.Add(MoveTemp(Waiter));
    }

    void FJobManager::Tick(const double NowSeconds)
    {
        TArray<TPair<FJobWaitCallback, FJobSnapshot>> ReadyWaiters;
        TArray<TSharedPtr<FImpl::FRecord, ESPMode::ThreadSafe>> ToStart;
        TArray<FJobProgress> TimeoutProgress;
        {
            FScopeLock Lock(&Impl->Mutex);
            FString ExpiredWriterId;
            const bool bWriteLeaseExpired = Impl->Lease.IsWriteExpired(NowSeconds, ExpiredWriterId);
            for (auto& Pair : Impl->Jobs)
            {
                FImpl::FRecord& Record = *Pair.Value;
                if (bWriteLeaseExpired && Record.Snapshot.JobId == ExpiredWriterId
                    && Record.bRunning && Record.Snapshot.Access == EJobAccess::Write && !Record.bLeaseTimedOut)
                {
                    Record.bLeaseTimedOut = true;
                    Record.Snapshot.bCancellationRequested = true;
                    Record.Snapshot.Phase = EJobPhase::Stopping;
                    Record.Snapshot.bCancellationSafe = false;
                    Record.Snapshot.UpdatedAt = FDateTime::UtcNow();
                    Record.Snapshot.Error = FProtocolError::Make(EErrorCode::WriteLeaseExpired,
                        TEXT("Write lease heartbeat expired; the job will fail after the worker stops and its asset state must be verified."),
                        TEXT("FJobManager::Tick"));
                    FJobProgress Progress;
                    Progress.JobId = Record.Snapshot.JobId;
                    Progress.Phase = EJobPhase::Stopping;
                    Progress.Message = TEXT("Write lease heartbeat expired; waiting for the worker to stop safely.");
                    Progress.Timestamp = Record.Snapshot.UpdatedAt;
                    Record.Snapshot.Progress.Add(Progress);
                    TimeoutProgress.Add(Progress);
                }
            }
            for (int32 Index = Impl->Waiters.Num() - 1; Index >= 0; --Index)
            {
                const TSharedPtr<FImpl::FRecord, ESPMode::ThreadSafe>* Record = Impl->Jobs.Find(Impl->Waiters[Index].JobId);
                if (Record == nullptr || (*Record)->Snapshot.bTerminal || NowSeconds >= Impl->Waiters[Index].DeadlineSeconds)
                {
                    FJobSnapshot Snapshot;
                    if (Record != nullptr) Snapshot = (*Record)->Snapshot;
                    ReadyWaiters.Emplace(MoveTemp(Impl->Waiters[Index].Callback), MoveTemp(Snapshot));
                    Impl->Waiters.RemoveAtSwap(Index);
                }
            }

            bool bWriterQueued = false;
            for (const FString& JobId : Impl->Queue)
            {
                const TSharedPtr<FImpl::FRecord, ESPMode::ThreadSafe>* Record = Impl->Jobs.Find(JobId);
                if (Record != nullptr && (*Record)->Snapshot.Access == EJobAccess::Write) { bWriterQueued = true; break; }
            }
            for (int32 Index = 0; Index < Impl->Queue.Num();)
            {
                const FString JobId = Impl->Queue[Index];
                TSharedPtr<FImpl::FRecord, ESPMode::ThreadSafe> Record = Impl->Jobs.FindRef(JobId);
                if (!Record.IsValid()) { Impl->Queue.RemoveAt(Index); continue; }
                bool bAcquired = false;
                if (Record->Snapshot.Access == EJobAccess::Write)
                {
                    bAcquired = Impl->Lease.TryAcquireWrite(JobId, NowSeconds);
                }
                else if (!bWriterQueued)
                {
                    bAcquired = Impl->Lease.TryAcquireRead(JobId);
                }
                if (!bAcquired) { ++Index; continue; }
                if (Record->bJournaled)
                {
                    FRequestJournalRecord JournalRecord;
                    FProtocolError JournalError;
                    if (!FRequestJournal::Get().MarkRunning(Record->Snapshot.RequestId, JournalRecord, JournalError))
                    {
                        Record->Snapshot.Phase = EJobPhase::Failed;
                        Record->Snapshot.bTerminal = true;
                        Record->Snapshot.Error = JournalError;
                        Record->Snapshot.UpdatedAt = FDateTime::UtcNow();
                        if (Record->Snapshot.Access == EJobAccess::Write) Impl->Lease.ReleaseWrite(JobId);
                        else Impl->Lease.ReleaseRead(JobId);
                        Impl->Queue.RemoveAt(Index);
                        continue;
                    }
                }
                Record->bRunning = true;
                Record->Snapshot.Phase = EJobPhase::Preflight;
                Record->Snapshot.bCancellationSafe = true;
                Record->Snapshot.UpdatedAt = FDateTime::UtcNow();
                Impl->Queue.RemoveAt(Index);
                ++Impl->ActiveWorkerCount;
                ToStart.Add(Record);
                if (Record->Snapshot.Access == EJobAccess::Write) break;
            }
        }

        for (TPair<FJobWaitCallback, FJobSnapshot>& Pair : ReadyWaiters) Pair.Key(Pair.Value);
        for (const FJobProgress& Progress : TimeoutProgress) Impl->ProgressDelegate.Broadcast(Progress);
        for (const TSharedPtr<FImpl::FRecord, ESPMode::ThreadSafe>& Record : ToStart)
        {
            Async(EAsyncExecution::ThreadPool, [this, Record]()
            {
                FJobExecutionContext Context(*this, Record->Snapshot.JobId);
                TSharedPtr<FJsonObject> Result;
                FProtocolError Error;
                const bool bSucceeded = Record->Work(Context, Result, Error);
                const bool bCancelled = IsCancellationRequested(Record->Snapshot.JobId);
                EJobAccess Access;
                {
                    FScopeLock Lock(&Impl->Mutex);
                    Access = Record->Snapshot.Access;
                    if (Record->bLeaseTimedOut)
                    {
                        if (!Result.IsValid()) Result = MakeShared<FJsonObject>();
                        Result->SetBoolField(TEXT("success"), false);
                        Result->SetBoolField(TEXT("partial"), true);
                        Result->SetBoolField(TEXT("stateUnknown"), true);
                    }
                    Record->Snapshot.Result = Result;
                    Record->Snapshot.Phase = Record->bLeaseTimedOut ? EJobPhase::Failed
                        : (bCancelled ? EJobPhase::Cancelled : (bSucceeded ? EJobPhase::Succeeded : EJobPhase::Failed));
                    Record->Snapshot.bTerminal = true;
                    Record->Snapshot.bCancellationDeferred = false;
                    Record->Snapshot.bCancellationSafe = true;
                    Record->Snapshot.UpdatedAt = FDateTime::UtcNow();
                    if (!bSucceeded && !bCancelled && !Record->bLeaseTimedOut) Record->Snapshot.Error = Error.Code == EErrorCode::None
                        ? FProtocolError::Make(EErrorCode::InternalError, TEXT("Job failed without an error."), TEXT("FJobManager::Worker")) : Error;
                    Record->bRunning = false;
                }
                if (Record->bJournaled)
                {
                    FRequestJournalRecord JournalRecord;
                    FProtocolError JournalWriteError;
                    const TOptional<FProtocolError> TerminalError = Record->Snapshot.Error;
                    if (!FRequestJournal::Get().MarkTerminal(
                        Record->Snapshot.RequestId, Record->Snapshot.Result, TerminalError,
                        Record->Snapshot.Phase, JournalRecord, JournalWriteError))
                    {
                        FScopeLock Lock(&Impl->Mutex);
                        Record->Snapshot.Phase = EJobPhase::Failed;
                        Record->Snapshot.Error = JournalWriteError;
                        Record->Snapshot.Result.Reset();
                    }
                }
                if (Access == EJobAccess::Write) Impl->Lease.ReleaseWrite(Record->Snapshot.JobId);
                else Impl->Lease.ReleaseRead(Record->Snapshot.JobId);
                {
                    FScopeLock Lock(&Impl->Mutex);
                    check(Impl->ActiveWorkerCount > 0);
                    --Impl->ActiveWorkerCount;
                }
            });
        }
    }

    void FJobManager::Shutdown()
    {
        if (!Impl.IsValid()) return;
        {
            FScopeLock Lock(&Impl->Mutex);
            Impl->bShuttingDown = true;
            for (auto& Pair : Impl->Jobs)
            {
                FImpl::FRecord& Record = *Pair.Value;
                if (Record.Snapshot.bTerminal) continue;
                Record.Snapshot.bCancellationRequested = true;
                Record.Snapshot.UpdatedAt = FDateTime::UtcNow();
                if (!Record.bRunning)
                {
                    if (Record.bJournaled)
                    {
                        FRequestJournalRecord JournalRecord;
                        FProtocolError JournalError;
                        if (!FRequestJournal::Get().MarkTerminal(Record.Snapshot.RequestId, nullptr,
                            TOptional<FProtocolError>(), EJobPhase::Cancelled, JournalRecord, JournalError))
                        {
                            Record.Snapshot.Phase = EJobPhase::Failed;
                            Record.Snapshot.Error = JournalError;
                        }
                        else Record.Snapshot.Phase = EJobPhase::Cancelled;
                    }
                    else Record.Snapshot.Phase = EJobPhase::Cancelled;
                    Record.Snapshot.bTerminal = true;
                    Record.Snapshot.bCancellationSafe = true;
                }
            }
            Impl->Queue.Reset();
        }

        // A write worker can be blocked waiting for its game-thread mutation. Pump those tasks before
        // unloading the module so no worker can continue executing code from an unloaded DLL.
        for (;;)
        {
            {
                FScopeLock Lock(&Impl->Mutex);
                if (Impl->ActiveWorkerCount == 0) break;
            }
            if (IsInGameThread() && FTaskGraphInterface::IsRunning())
                FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
            FPlatformProcess::Sleep(0.001f);
        }
    }

    FOnJobProgress& FJobManager::OnProgress() { return Impl->ProgressDelegate; }

    bool FJobManager::EnterPhase(const FString& JobId, const EJobPhase Phase, const bool bCancellationSafe, const FString& Message)
    {
        FJobProgress Progress;
        {
            FScopeLock Lock(&Impl->Mutex);
            TSharedPtr<FImpl::FRecord, ESPMode::ThreadSafe> Record = Impl->Jobs.FindRef(JobId);
            if (!Record.IsValid() || Record->Snapshot.bTerminal) return false;
            if (bCancellationSafe && Record->Snapshot.bCancellationDeferred)
            {
                Record->Snapshot.bCancellationDeferred = false;
                Record->Snapshot.bCancellationRequested = true;
                Record->Snapshot.UpdatedAt = FDateTime::UtcNow();
                return false;
            }
            if (Record->Snapshot.bCancellationRequested && bCancellationSafe) return false;
            Record->Snapshot.Phase = Phase;
            Record->Snapshot.bCancellationSafe = bCancellationSafe;
            Record->Snapshot.UpdatedAt = FDateTime::UtcNow();
            if (Record->Snapshot.Access == EJobAccess::Write)
                Impl->Lease.HeartbeatWrite(JobId, FPlatformTime::Seconds());
            Progress.JobId = JobId;
            Progress.Phase = Phase;
            Progress.Message = Message;
            Progress.Timestamp = Record->Snapshot.UpdatedAt;
            Record->Snapshot.Progress.Add(Progress);
        }
        Impl->ProgressDelegate.Broadcast(Progress);
        return true;
    }

    void FJobManager::ReportProgress(const FString& JobId, const int32 Completed, const int32 Total, const FString& Message, const FString& AssetPath)
    {
        FJobProgress Progress;
        {
            FScopeLock Lock(&Impl->Mutex);
            TSharedPtr<FImpl::FRecord, ESPMode::ThreadSafe> Record = Impl->Jobs.FindRef(JobId);
            if (!Record.IsValid() || Record->Snapshot.bTerminal) return;
            Progress.JobId = JobId;
            Progress.Phase = Record->Snapshot.Phase;
            Progress.Completed = FMath::Max(0, Completed);
            Progress.Total = FMath::Max(0, Total);
            Progress.Message = Message;
            Progress.AssetPath = AssetPath;
            Progress.Timestamp = FDateTime::UtcNow();
            Record->Snapshot.Progress.Add(Progress);
            Record->Snapshot.UpdatedAt = Progress.Timestamp;
            if (Record->Snapshot.Access == EJobAccess::Write)
                Impl->Lease.HeartbeatWrite(JobId, FPlatformTime::Seconds());
        }
        Impl->ProgressDelegate.Broadcast(Progress);
    }

    void FJobManager::Heartbeat(const FString& JobId)
    {
        FScopeLock Lock(&Impl->Mutex);
        TSharedPtr<FImpl::FRecord, ESPMode::ThreadSafe> Record = Impl->Jobs.FindRef(JobId);
        if (!Record.IsValid()) return;
        if (Record->Snapshot.Access == EJobAccess::Write)
            Impl->Lease.HeartbeatWrite(JobId, FPlatformTime::Seconds());
    }

    bool FJobManager::IsCancellationRequested(const FString& JobId) const
    {
        FScopeLock Lock(&Impl->Mutex);
        const TSharedPtr<FImpl::FRecord, ESPMode::ThreadSafe> Record = Impl->Jobs.FindRef(JobId);
        return !Record.IsValid() || Record->Snapshot.bCancellationRequested;
    }
}
