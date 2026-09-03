#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "CodexUnrealBlueprintProtocol.h"

namespace CodexUnrealBlueprint
{
    enum class ERequestJournalState : uint8
    {
        Accepted,
        Running,
        Terminal
    };

    enum class ERequestAcceptResult : uint8
    {
        Accepted,
        Replay,
        Conflict,
        Failed
    };

    struct CODEXUNREALBLUEPRINTCORE_API FRequestJournalRecord
    {
        FString RequestId;
        FString Method;
        FString PayloadHash;
        FString JobId;
        ERequestJournalState State = ERequestJournalState::Accepted;
        EJobPhase TerminalPhase = EJobPhase::Succeeded;
        FDateTime AcceptedAt;
        FDateTime UpdatedAt;
        bool bInterrupted = false;
        bool bRecoveryRequired = false;
        TSharedPtr<FJsonObject> Result;
        TOptional<FProtocolError> Error;

        TSharedRef<FJsonObject> ToJson() const;
        TSharedRef<FJsonObject> ToStatusJson() const;
    };

    struct CODEXUNREALBLUEPRINTCORE_API FRequestJournalStatus
    {
        int32 RecordCount = 0;
        int32 AcceptedCount = 0;
        int32 RunningCount = 0;
        int32 TerminalCount = 0;
        bool bHealthy = true;
        FString Directory;
        TOptional<FProtocolError> Error;

        TSharedRef<FJsonObject> ToJson() const;
    };

    class CODEXUNREALBLUEPRINTCORE_API FRequestJournal
    {
    public:
        explicit FRequestJournal(const FString& DirectoryOverride = FString(), int32 InMaxCacheEntries = 256);
        ~FRequestJournal();

        FRequestJournal(const FRequestJournal&) = delete;
        FRequestJournal& operator=(const FRequestJournal&) = delete;

        static FRequestJournal& Get();
        static FString HashCanonicalParams(const TSharedPtr<FJsonObject>& Params);

        bool Initialize(FProtocolError& OutError);
        ERequestAcceptResult Accept(
            const FString& RequestId,
            const FString& Method,
            const TSharedPtr<FJsonObject>& Params,
            const FString& JobId,
            FRequestJournalRecord& OutRecord,
            FProtocolError& OutError);
        bool MarkRunning(const FString& RequestId, FRequestJournalRecord& OutRecord, FProtocolError& OutError);
        bool MarkTerminal(
            const FString& RequestId,
            const TSharedPtr<FJsonObject>& Result,
            const TOptional<FProtocolError>& Error,
            FRequestJournalRecord& OutRecord,
            FProtocolError& OutError);
        bool MarkTerminal(
            const FString& RequestId,
            const TSharedPtr<FJsonObject>& Result,
            const TOptional<FProtocolError>& Error,
            EJobPhase TerminalPhase,
            FRequestJournalRecord& OutRecord,
            FProtocolError& OutError);
        bool Query(const FString& RequestId, FRequestJournalRecord& OutRecord, FProtocolError& OutError) const;
        bool GetStatus(FRequestJournalStatus& OutStatus, FProtocolError& OutError) const;
        bool GetAll(TArray<FRequestJournalRecord>& OutRecords, FProtocolError& OutError) const;
        FString GetDirectory() const;

    private:
        FString RecordPath(const FString& RequestId) const;
        bool ReadRecordFile(const FString& Path, FRequestJournalRecord& OutRecord, FProtocolError& OutError) const;
        bool WriteRecordFile(const FRequestJournalRecord& Record, FProtocolError& OutError) const;
        bool EnsureReady(FProtocolError& OutError) const;
        bool LockAcrossProcesses(const TCHAR* Callsite, FProtocolError& OutError) const;
        void UnlockAcrossProcesses() const;
        void CacheRecord(const FRequestJournalRecord& Record) const;

        FString Directory;
        int32 MaxCacheEntries;
        mutable FCriticalSection Mutex;
        void* InterprocessMutexHandle = nullptr;
        FString InterprocessMutexName;
        FString InterprocessMutexInitializationError;
        mutable TMap<FString, FRequestJournalRecord> Cache;
        mutable TArray<FString> CacheOrder;
        bool bInitialized = false;
        TOptional<FProtocolError> InitializationError;
    };
}
