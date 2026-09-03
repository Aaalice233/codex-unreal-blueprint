#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "PiUnrealBlueprintProtocol.h"

namespace PiUnrealBlueprint
{
    enum class EJobAccess : uint8
    {
        Read,
        Write
    };

    struct PIUNREALBLUEPRINTCORE_API FJobProgress
    {
        FString JobId;
        EJobPhase Phase = EJobPhase::Queued;
        int32 Completed = 0;
        int32 Total = 0;
        FString Message;
        FString AssetPath;
        FDateTime Timestamp;

        TSharedRef<FJsonObject> ToJson() const;
    };

    struct PIUNREALBLUEPRINTCORE_API FJobManagerStatus
    {
        int32 JobCount = 0;
        int32 QueuedCount = 0;
        int32 RunningCount = 0;
        int32 TerminalCount = 0;
        int32 QueuedReadCount = 0;
        int32 QueuedWriteCount = 0;

        TSharedRef<FJsonObject> ToJson() const;
    };

    struct PIUNREALBLUEPRINTCORE_API FJobSnapshot
    {
        FString JobId;
        FString RequestId;
        EJobAccess Access = EJobAccess::Read;
        EJobPhase Phase = EJobPhase::Queued;
        bool bTerminal = false;
        bool bCancellationRequested = false;
        bool bCancellationDeferred = false;
        bool bCancellationSafe = true;
        FDateTime CreatedAt;
        FDateTime UpdatedAt;
        TOptional<FProtocolError> Error;
        TSharedPtr<FJsonObject> Result;
        TArray<FJobProgress> Progress;

        TSharedRef<FJsonObject> ToJson() const;
    };

    class FJobManager;

    class PIUNREALBLUEPRINTCORE_API FJobExecutionContext
    {
    public:
        bool EnterPhase(EJobPhase Phase, bool bCancellationSafe, const FString& Message = FString());
        void ReportProgress(int32 Completed, int32 Total, const FString& Message, const FString& AssetPath = FString());
        void Heartbeat();
        bool IsCancellationRequested() const;
        FString GetJobId() const;

    private:
        friend class FJobManager;
        FJobExecutionContext(FJobManager& InManager, const FString& InJobId);
        FJobManager& Manager;
        FString JobId;
    };

    using FJobWork = TFunction<bool(FJobExecutionContext&, TSharedPtr<FJsonObject>&, FProtocolError&)>;
    using FJobWaitCallback = TFunction<void(const FJobSnapshot&)>;
    DECLARE_MULTICAST_DELEGATE_OneParam(FOnJobProgress, const FJobProgress&);

    class PIUNREALBLUEPRINTCORE_API FReadWriteLease
    {
    public:
        explicit FReadWriteLease(double InWriteTimeoutSeconds = 15.0);

        bool TryAcquireRead(const FString& OwnerId);
        void ReleaseRead(const FString& OwnerId);
        bool TryAcquireWrite(const FString& OwnerId, double NowSeconds);
        bool HeartbeatWrite(const FString& OwnerId, double NowSeconds);
        void ReleaseWrite(const FString& OwnerId);
        bool IsWriteExpired(double NowSeconds, FString& OutExpiredOwnerId) const;
        int32 GetReaderCount() const;
        FString GetWriterId() const;

    private:
        mutable FCriticalSection Mutex;
        TSet<FString> Readers;
        FString WriterId;
        double WriterHeartbeatSeconds = 0.0;
        double WriteTimeoutSeconds;
    };

    class PIUNREALBLUEPRINTCORE_API FJobManager
    {
    public:
        static FJobManager& Get();
        ~FJobManager();

        static constexpr int32 MaxQueuedJobs = 256;

        FString Start(EJobAccess Access, const FString& RequestId, FJobWork Work, FProtocolError* OutError = nullptr);
        bool StartWrite(
            const FString& Method,
            const FString& RequestId,
            const TSharedPtr<FJsonObject>& Params,
            FJobWork Work,
            FJobSnapshot& OutSnapshot,
            FProtocolError& OutError,
            bool& bOutReplay);
        bool Get(const FString& JobId, FJobSnapshot& OutSnapshot) const;
        bool FindByRequestId(const FString& RequestId, FJobSnapshot& OutSnapshot) const;
        void GetSnapshots(TArray<FJobSnapshot>& OutSnapshots) const;
        FJobManagerStatus GetStatus() const;
        bool Cancel(const FString& JobId, FJobSnapshot& OutSnapshot, FProtocolError& OutError);
        void Wait(const FString& JobId, double TimeoutSeconds, FJobWaitCallback Callback);
        void Tick(double NowSeconds);
        void Shutdown();
        FOnJobProgress& OnProgress();

        bool EnterPhase(const FString& JobId, EJobPhase Phase, bool bCancellationSafe, const FString& Message);
        void ReportProgress(const FString& JobId, int32 Completed, int32 Total, const FString& Message, const FString& AssetPath);
        void Heartbeat(const FString& JobId);
        bool IsCancellationRequested(const FString& JobId) const;

    private:
        FJobManager();
        struct FImpl;
        TUniquePtr<FImpl> Impl;
    };
}
