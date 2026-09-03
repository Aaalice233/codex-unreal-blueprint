#include "PiUnrealBlueprintRequestJournal.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace PiUnrealBlueprint
{
    namespace
    {
        const TCHAR* StateToString(const ERequestJournalState State)
        {
            switch (State)
            {
            case ERequestJournalState::Accepted: return TEXT("accepted");
            case ERequestJournalState::Running: return TEXT("running");
            case ERequestJournalState::Terminal: return TEXT("terminal");
            default: return TEXT("unknown");
            }
        }

        bool ParseState(const FString& Value, ERequestJournalState& OutState)
        {
            if (Value == TEXT("accepted")) OutState = ERequestJournalState::Accepted;
            else if (Value == TEXT("running")) OutState = ERequestJournalState::Running;
            else if (Value == TEXT("terminal")) OutState = ERequestJournalState::Terminal;
            else return false;
            return true;
        }

        bool ParseTerminalPhase(const FString& Value, EJobPhase& OutPhase)
        {
            if (Value == TEXT("Succeeded")) OutPhase = EJobPhase::Succeeded;
            else if (Value == TEXT("Failed")) OutPhase = EJobPhase::Failed;
            else if (Value == TEXT("Cancelled")) OutPhase = EJobPhase::Cancelled;
            else return false;
            return true;
        }

        FString QuoteJsonString(const FString& Value)
        {
            FString Output;
            const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
            Writer->WriteValue(Value);
            Writer->Close();
            return Output;
        }

        void AppendCanonical(const TSharedPtr<FJsonValue>& Value, FString& Out)
        {
            if (!Value.IsValid() || Value->Type == EJson::Null)
            {
                Out += TEXT("null");
                return;
            }
            switch (Value->Type)
            {
            case EJson::String:
                Out += QuoteJsonString(Value->AsString());
                break;
            case EJson::Number:
                Out += FString::Printf(TEXT("%.17g"), Value->AsNumber());
                break;
            case EJson::Boolean:
                Out += Value->AsBool() ? TEXT("true") : TEXT("false");
                break;
            case EJson::Array:
            {
                Out += TEXT("[");
                const TArray<TSharedPtr<FJsonValue>>& Values = Value->AsArray();
                for (int32 Index = 0; Index < Values.Num(); ++Index)
                {
                    if (Index > 0) Out += TEXT(",");
                    AppendCanonical(Values[Index], Out);
                }
                Out += TEXT("]");
                break;
            }
            case EJson::Object:
            {
                Out += TEXT("{");
                TArray<FString> Keys;
                Value->AsObject()->Values.GetKeys(Keys);
                Keys.Sort();
                for (int32 Index = 0; Index < Keys.Num(); ++Index)
                {
                    if (Index > 0) Out += TEXT(",");
                    Out += QuoteJsonString(Keys[Index]);
                    Out += TEXT(":");
                    AppendCanonical(Value->AsObject()->Values.FindRef(Keys[Index]), Out);
                }
                Out += TEXT("}");
                break;
            }
            default:
                Out += TEXT("null");
                break;
            }
        }

        FString SerializeObject(const TSharedRef<FJsonObject>& Object)
        {
            FString Output;
            const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
            FJsonSerializer::Serialize(Object, Writer);
            return Output;
        }

        FString JournalDigestToLowerHex(const uint8* Digest, const int32 DigestSize)
        {
            return ::BytesToHex(Digest, DigestSize).ToLower();
        }

        bool ParseError(const TSharedPtr<FJsonObject>& Json, FProtocolError& OutError)
        {
            FString Code;
            if (!Json.IsValid() || !Json->TryGetStringField(TEXT("code"), Code)
                || !Json->TryGetStringField(TEXT("message"), OutError.Message)
                || !Json->TryGetStringField(TEXT("ueCallsite"), OutError.UECallsite))
            {
                return false;
            }
            bool bKnownCode = false;
            for (int32 Raw = static_cast<int32>(EErrorCode::None); Raw <= static_cast<int32>(EErrorCode::InternalError); ++Raw)
            {
                const EErrorCode Candidate = static_cast<EErrorCode>(Raw);
                if (Code == LexToString(Candidate))
                {
                    OutError.Code = Candidate;
                    bKnownCode = true;
                    break;
                }
            }
            if (!bKnownCode) return false;
            Json->TryGetStringField(TEXT("assetPath"), OutError.AssetPath);
            double OperationIndex = 0.0;
            if (Json->TryGetNumberField(TEXT("operationIndex"), OperationIndex)) OutError.OperationIndex = static_cast<int32>(OperationIndex);
            const TArray<TSharedPtr<FJsonValue>>* Messages = nullptr;
            if (Json->TryGetArrayField(TEXT("compilerMessages"), Messages) && Messages != nullptr)
            {
                for (const TSharedPtr<FJsonValue>& Message : *Messages)
                {
                    if (Message.IsValid() && Message->Type == EJson::String) OutError.CompilerMessages.Add(Message->AsString());
                }
            }
            return true;
        }

        FProtocolError JournalError(const EErrorCode Code, const FString& Message, const FString& Callsite)
        {
            return FProtocolError::Make(Code, Message, Callsite);
        }

        bool IsLowerHexHash(const FString& Value)
        {
            if (Value.Len() != FSHA1::DigestSize * 2) return false;
            for (const TCHAR Character : Value)
            {
                if (!FChar::IsHexDigit(Character) || (Character >= TEXT('A') && Character <= TEXT('F'))) return false;
            }
            return true;
        }
    }

    TSharedRef<FJsonObject> FRequestJournalRecord::ToJson() const
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetNumberField(TEXT("version"), 1);
        Json->SetStringField(TEXT("requestId"), RequestId);
        Json->SetStringField(TEXT("method"), Method);
        Json->SetStringField(TEXT("payloadHash"), PayloadHash);
        Json->SetStringField(TEXT("jobId"), JobId);
        Json->SetStringField(TEXT("state"), StateToString(State));
        if (State == ERequestJournalState::Terminal)
        {
            Json->SetStringField(TEXT("terminalPhase"), LexToString(TerminalPhase));
        }
        Json->SetStringField(TEXT("acceptedAt"), AcceptedAt.ToIso8601());
        Json->SetStringField(TEXT("updatedAt"), UpdatedAt.ToIso8601());
        Json->SetBoolField(TEXT("interrupted"), bInterrupted);
        Json->SetBoolField(TEXT("recoveryRequired"), bRecoveryRequired);
        if (Result.IsValid()) Json->SetObjectField(TEXT("result"), Result.ToSharedRef());
        if (Error.IsSet()) Json->SetObjectField(TEXT("error"), Error.GetValue().ToJson());
        return Json;
    }

    TSharedRef<FJsonObject> FRequestJournalRecord::ToStatusJson() const
    {
        return ToJson();
    }

    TSharedRef<FJsonObject> FRequestJournalStatus::ToJson() const
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetBoolField(TEXT("healthy"), bHealthy);
        Json->SetStringField(TEXT("directory"), Directory);
        Json->SetNumberField(TEXT("recordCount"), RecordCount);
        Json->SetNumberField(TEXT("accepted"), AcceptedCount);
        Json->SetNumberField(TEXT("running"), RunningCount);
        Json->SetNumberField(TEXT("terminal"), TerminalCount);
        if (Error.IsSet()) Json->SetObjectField(TEXT("error"), Error.GetValue().ToJson());
        return Json;
    }

    FRequestJournal::FRequestJournal(const FString& DirectoryOverride, const int32 InMaxCacheEntries)
        : Directory(DirectoryOverride.IsEmpty()
            ? FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("PiUnrealBlueprint"))
            : FPaths::ConvertRelativePathToFull(DirectoryOverride))
        , MaxCacheEntries(FMath::Max(1, InMaxCacheEntries))
    {
        FPaths::NormalizeDirectoryName(Directory);
        FString CanonicalDirectory = Directory.ToLower();
        FTCHARToUTF8 Utf8(*CanonicalDirectory);
        uint8 Digest[FSHA1::DigestSize];
        FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Digest);
        InterprocessMutexName = TEXT("Local\\PiUnrealBlueprint.RequestJournal.")
            + JournalDigestToLowerHex(Digest, FSHA1::DigestSize);
#if PLATFORM_WINDOWS
        InterprocessMutexHandle = CreateMutex(nullptr, false, *InterprocessMutexName);
        if (InterprocessMutexHandle == nullptr)
        {
            InterprocessMutexInitializationError = FString::Printf(
                TEXT("CreateMutex failed for request journal '%s' with Windows error %lu."),
                *Directory, static_cast<uint32>(GetLastError()));
        }
#else
        InterprocessMutexInitializationError = TEXT("Cross-process request journal locking requires Windows.");
#endif
    }

    FRequestJournal::~FRequestJournal()
    {
#if PLATFORM_WINDOWS
        if (InterprocessMutexHandle != nullptr)
        {
            CloseHandle(static_cast<HANDLE>(InterprocessMutexHandle));
            InterprocessMutexHandle = nullptr;
        }
#endif
    }

    FRequestJournal& FRequestJournal::Get()
    {
        static FRequestJournal Journal;
        return Journal;
    }

    FString FRequestJournal::HashCanonicalParams(const TSharedPtr<FJsonObject>& Params)
    {
        FString Canonical;
        AppendCanonical(MakeShared<FJsonValueObject>(Params.IsValid() ? Params.ToSharedRef() : MakeShared<FJsonObject>()), Canonical);
        FTCHARToUTF8 Utf8(*Canonical);
        uint8 Digest[FSHA1::DigestSize];
        FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Digest);
        return JournalDigestToLowerHex(Digest, FSHA1::DigestSize);
    }

    bool FRequestJournal::Initialize(FProtocolError& OutError)
    {
        FScopeLock Lock(&Mutex);
        if (bInitialized) return EnsureReady(OutError);
        if (!LockAcrossProcesses(TEXT("FRequestJournal::Initialize"), OutError))
        {
            InitializationError = OutError;
            bInitialized = true;
            return false;
        }
        ON_SCOPE_EXIT { UnlockAcrossProcesses(); };
        if (!IFileManager::Get().MakeDirectory(*Directory, true))
        {
            InitializationError = JournalError(EErrorCode::JournalIoError,
                FString::Printf(TEXT("Cannot create request journal directory '%s'."), *Directory), TEXT("FRequestJournal::Initialize"));
            OutError = InitializationError.GetValue();
            bInitialized = true;
            return false;
        }

        TArray<FString> Files;
        IFileManager::Get().FindFiles(Files, *FPaths::Combine(Directory, TEXT("*.json")), true, false);
        TArray<FRequestJournalRecord> Records;
        for (const FString& File : Files)
        {
            FRequestJournalRecord Record;
            FProtocolError Error;
            const FString Path = FPaths::Combine(Directory, File);
            if (!ReadRecordFile(Path, Record, Error))
            {
                InitializationError = Error;
                OutError = Error;
                bInitialized = true;
                return false;
            }
            if (FPaths::GetCleanFilename(RecordPath(Record.RequestId)) != File)
            {
                Error = JournalError(EErrorCode::JournalCorrupt,
                    FString::Printf(TEXT("Request journal '%s' does not match its requestId."), *Path), TEXT("FRequestJournal::Initialize"));
                InitializationError = Error;
                OutError = Error;
                bInitialized = true;
                return false;
            }
            Records.Add(MoveTemp(Record));
        }

        for (FRequestJournalRecord& Record : Records)
        {
            if (Record.State != ERequestJournalState::Terminal)
            {
                Record.State = ERequestJournalState::Terminal;
                Record.TerminalPhase = EJobPhase::Failed;
                Record.bInterrupted = true;
                Record.bRecoveryRequired = true;
                Record.UpdatedAt = FDateTime::UtcNow();
                Record.Result.Reset();
                Record.Error = JournalError(EErrorCode::RequestInterrupted,
                    TEXT("The Editor process stopped while this write was in flight; request-state reconciliation is required before retrying. This flag does not indicate that an asset backup exists or that automatic asset recovery is available."),
                    TEXT("FRequestJournal::Initialize"));
                FProtocolError WriteError;
                if (!WriteRecordFile(Record, WriteError))
                {
                    InitializationError = WriteError;
                    OutError = WriteError;
                    bInitialized = true;
                    return false;
                }
            }
            CacheRecord(Record);
        }
        bInitialized = true;
        OutError = FProtocolError();
        return true;
    }

    ERequestAcceptResult FRequestJournal::Accept(
        const FString& RequestId,
        const FString& Method,
        const TSharedPtr<FJsonObject>& Params,
        const FString& JobId,
        FRequestJournalRecord& OutRecord,
        FProtocolError& OutError)
    {
        FScopeLock Lock(&Mutex);
        if (!EnsureReady(OutError)) return ERequestAcceptResult::Failed;
        if (!LockAcrossProcesses(TEXT("FRequestJournal::Accept"), OutError)) return ERequestAcceptResult::Failed;
        ON_SCOPE_EXIT { UnlockAcrossProcesses(); };
        if (RequestId.TrimStartAndEnd().IsEmpty() || Method.IsEmpty() || JobId.IsEmpty())
        {
            OutError = JournalError(EErrorCode::RequestIdRequired, TEXT("Write requests require non-empty requestId, method, and jobId."), TEXT("FRequestJournal::Accept"));
            return ERequestAcceptResult::Failed;
        }
        const FString PayloadHash = HashCanonicalParams(Params);
        const FString Path = RecordPath(RequestId);
        if (IFileManager::Get().FileExists(*Path))
        {
            if (!ReadRecordFile(Path, OutRecord, OutError)) return ERequestAcceptResult::Failed;
            CacheRecord(OutRecord);
            if (OutRecord.RequestId != RequestId || OutRecord.Method != Method || OutRecord.PayloadHash != PayloadHash)
            {
                OutError = JournalError(EErrorCode::RequestConflict,
                    TEXT("requestId is already bound to a different method or canonical payload."), TEXT("FRequestJournal::Accept"));
                return ERequestAcceptResult::Conflict;
            }
            OutError = FProtocolError();
            return ERequestAcceptResult::Replay;
        }

        OutRecord = FRequestJournalRecord();
        OutRecord.RequestId = RequestId;
        OutRecord.Method = Method;
        OutRecord.PayloadHash = PayloadHash;
        OutRecord.JobId = JobId;
        OutRecord.State = ERequestJournalState::Accepted;
        OutRecord.AcceptedAt = FDateTime::UtcNow();
        OutRecord.UpdatedAt = OutRecord.AcceptedAt;
        if (!WriteRecordFile(OutRecord, OutError)) return ERequestAcceptResult::Failed;
        CacheRecord(OutRecord);
        return ERequestAcceptResult::Accepted;
    }

    bool FRequestJournal::MarkRunning(const FString& RequestId, FRequestJournalRecord& OutRecord, FProtocolError& OutError)
    {
        FScopeLock Lock(&Mutex);
        if (!EnsureReady(OutError)) return false;
        if (!LockAcrossProcesses(TEXT("FRequestJournal::MarkRunning"), OutError)) return false;
        ON_SCOPE_EXIT { UnlockAcrossProcesses(); };
        if (!ReadRecordFile(RecordPath(RequestId), OutRecord, OutError)) return false;
        if (OutRecord.State == ERequestJournalState::Terminal)
        {
            OutError = JournalError(EErrorCode::RequestConflict, TEXT("A terminal request cannot return to running."), TEXT("FRequestJournal::MarkRunning"));
            return false;
        }
        OutRecord.State = ERequestJournalState::Running;
        OutRecord.UpdatedAt = FDateTime::UtcNow();
        if (!WriteRecordFile(OutRecord, OutError)) return false;
        CacheRecord(OutRecord);
        return true;
    }

    bool FRequestJournal::MarkTerminal(
        const FString& RequestId,
        const TSharedPtr<FJsonObject>& Result,
        const TOptional<FProtocolError>& Error,
        FRequestJournalRecord& OutRecord,
        FProtocolError& OutError)
    {
        return MarkTerminal(RequestId, Result, Error,
            Error.IsSet() ? EJobPhase::Failed : EJobPhase::Succeeded, OutRecord, OutError);
    }

    bool FRequestJournal::MarkTerminal(
        const FString& RequestId,
        const TSharedPtr<FJsonObject>& Result,
        const TOptional<FProtocolError>& Error,
        const EJobPhase TerminalPhase,
        FRequestJournalRecord& OutRecord,
        FProtocolError& OutError)
    {
        FScopeLock Lock(&Mutex);
        if (!EnsureReady(OutError)) return false;
        if (!LockAcrossProcesses(TEXT("FRequestJournal::MarkTerminal"), OutError)) return false;
        ON_SCOPE_EXIT { UnlockAcrossProcesses(); };
        if (!ReadRecordFile(RecordPath(RequestId), OutRecord, OutError)) return false;
        if (OutRecord.State == ERequestJournalState::Terminal)
        {
            OutError = FProtocolError();
            CacheRecord(OutRecord);
            return true;
        }
        if (TerminalPhase != EJobPhase::Succeeded && TerminalPhase != EJobPhase::Failed
            && TerminalPhase != EJobPhase::Cancelled)
        {
            OutError = JournalError(EErrorCode::InvalidArgument, TEXT("Request journal terminal phase must be succeeded, failed, or cancelled."), TEXT("FRequestJournal::MarkTerminal"));
            return false;
        }
        OutRecord.State = ERequestJournalState::Terminal;
        OutRecord.TerminalPhase = TerminalPhase;
        OutRecord.UpdatedAt = FDateTime::UtcNow();
        OutRecord.bInterrupted = false;
        OutRecord.bRecoveryRequired = false;
        OutRecord.Result = Result;
        OutRecord.Error = Error;
        if (!WriteRecordFile(OutRecord, OutError)) return false;
        CacheRecord(OutRecord);
        return true;
    }

    bool FRequestJournal::Query(const FString& RequestId, FRequestJournalRecord& OutRecord, FProtocolError& OutError) const
    {
        FScopeLock Lock(&Mutex);
        if (!EnsureReady(OutError)) return false;
        if (!LockAcrossProcesses(TEXT("FRequestJournal::Query"), OutError)) return false;
        ON_SCOPE_EXIT { UnlockAcrossProcesses(); };
        const FString Path = RecordPath(RequestId);
        if (!IFileManager::Get().FileExists(*Path))
        {
            OutError = JournalError(EErrorCode::RequestNotFound, TEXT("Unknown requestId."), TEXT("FRequestJournal::Query"));
            return false;
        }
        if (!ReadRecordFile(Path, OutRecord, OutError)) return false;
        CacheRecord(OutRecord);
        return true;
    }

    bool FRequestJournal::GetStatus(FRequestJournalStatus& OutStatus, FProtocolError& OutError) const
    {
        OutStatus = FRequestJournalStatus();
        OutStatus.Directory = Directory;
        TArray<FRequestJournalRecord> Records;
        if (!GetAll(Records, OutError))
        {
            OutStatus.bHealthy = false;
            OutStatus.Error = OutError;
            return false;
        }
        OutStatus.RecordCount = Records.Num();
        for (const FRequestJournalRecord& Record : Records)
        {
            if (Record.State == ERequestJournalState::Accepted) ++OutStatus.AcceptedCount;
            else if (Record.State == ERequestJournalState::Running) ++OutStatus.RunningCount;
            else ++OutStatus.TerminalCount;
        }
        return true;
    }

    bool FRequestJournal::GetAll(TArray<FRequestJournalRecord>& OutRecords, FProtocolError& OutError) const
    {
        FScopeLock Lock(&Mutex);
        OutRecords.Reset();
        if (!EnsureReady(OutError)) return false;
        if (!LockAcrossProcesses(TEXT("FRequestJournal::GetAll"), OutError)) return false;
        ON_SCOPE_EXIT { UnlockAcrossProcesses(); };
        TArray<FString> Files;
        IFileManager::Get().FindFiles(Files, *FPaths::Combine(Directory, TEXT("*.json")), true, false);
        for (const FString& File : Files)
        {
            FRequestJournalRecord Record;
            const FString Path = FPaths::Combine(Directory, File);
            if (!ReadRecordFile(Path, Record, OutError)) return false;
            if (FPaths::GetCleanFilename(RecordPath(Record.RequestId)) != File)
            {
                OutError = JournalError(EErrorCode::JournalCorrupt,
                    FString::Printf(TEXT("Request journal '%s' does not match its requestId."), *Path), TEXT("FRequestJournal::GetAll"));
                OutRecords.Reset();
                return false;
            }
            OutRecords.Add(MoveTemp(Record));
        }
        return true;
    }

    FString FRequestJournal::GetDirectory() const { return Directory; }

    FString FRequestJournal::RecordPath(const FString& RequestId) const
    {
        FTCHARToUTF8 Utf8(*RequestId);
        uint8 Digest[FSHA1::DigestSize];
        FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Digest);
        return FPaths::Combine(Directory, JournalDigestToLowerHex(Digest, FSHA1::DigestSize) + TEXT(".json"));
    }

    bool FRequestJournal::ReadRecordFile(const FString& Path, FRequestJournalRecord& OutRecord, FProtocolError& OutError) const
    {
        OutRecord = FRequestJournalRecord();
        FString Content;
        TSharedPtr<FJsonObject> Json;
        if (!FFileHelper::LoadFileToString(Content, *Path))
        {
            OutError = JournalError(EErrorCode::JournalIoError,
                FString::Printf(TEXT("Request journal '%s' cannot be read."), *Path), TEXT("FRequestJournal::ReadRecordFile"));
            return false;
        }
        if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Content), Json) || !Json.IsValid())
        {
            OutError = JournalError(EErrorCode::JournalCorrupt,
                FString::Printf(TEXT("Request journal '%s' contains invalid JSON."), *Path), TEXT("FRequestJournal::ReadRecordFile"));
            return false;
        }
        double Version = 0.0;
        FString State;
        if (!Json->TryGetNumberField(TEXT("version"), Version) || Version != 1.0
            || !Json->TryGetStringField(TEXT("requestId"), OutRecord.RequestId) || OutRecord.RequestId.IsEmpty()
            || !Json->TryGetStringField(TEXT("method"), OutRecord.Method) || OutRecord.Method.IsEmpty()
            || (!Json->TryGetStringField(TEXT("payloadHash"), OutRecord.PayloadHash)
                && !Json->TryGetStringField(TEXT("paramsHash"), OutRecord.PayloadHash))
            || !IsLowerHexHash(OutRecord.PayloadHash)
            || !Json->TryGetStringField(TEXT("jobId"), OutRecord.JobId) || OutRecord.JobId.IsEmpty()
            || !Json->TryGetStringField(TEXT("state"), State) || !ParseState(State, OutRecord.State))
        {
            OutError = JournalError(EErrorCode::JournalCorrupt,
                FString::Printf(TEXT("Request journal '%s' has an invalid schema."), *Path), TEXT("FRequestJournal::ReadRecordFile"));
            return false;
        }
        FString AcceptedAt;
        FString UpdatedAt;
        if (!Json->TryGetStringField(TEXT("acceptedAt"), AcceptedAt) || !FDateTime::ParseIso8601(*AcceptedAt, OutRecord.AcceptedAt)
            || !Json->TryGetStringField(TEXT("updatedAt"), UpdatedAt) || !FDateTime::ParseIso8601(*UpdatedAt, OutRecord.UpdatedAt)
            || !Json->TryGetBoolField(TEXT("interrupted"), OutRecord.bInterrupted)
            || !Json->TryGetBoolField(TEXT("recoveryRequired"), OutRecord.bRecoveryRequired))
        {
            OutError = JournalError(EErrorCode::JournalCorrupt,
                FString::Printf(TEXT("Request journal '%s' has invalid state metadata."), *Path), TEXT("FRequestJournal::ReadRecordFile"));
            return false;
        }
        if (OutRecord.State == ERequestJournalState::Terminal)
        {
            FString TerminalPhase;
            if (Json->TryGetStringField(TEXT("terminalPhase"), TerminalPhase))
            {
                if (!ParseTerminalPhase(TerminalPhase, OutRecord.TerminalPhase))
                {
                    OutError = JournalError(EErrorCode::JournalCorrupt,
                        FString::Printf(TEXT("Request journal '%s' has an invalid terminal phase."), *Path), TEXT("FRequestJournal::ReadRecordFile"));
                    return false;
                }
            }
            else
            {
                OutRecord.TerminalPhase = Json->HasField(TEXT("error")) ? EJobPhase::Failed : EJobPhase::Succeeded;
            }
        }
        const TSharedPtr<FJsonObject>* Result = nullptr;
        if (Json->HasField(TEXT("result")))
        {
            if (!Json->TryGetObjectField(TEXT("result"), Result) || Result == nullptr)
            {
                OutError = JournalError(EErrorCode::JournalCorrupt,
                    FString::Printf(TEXT("Request journal '%s' has an invalid result object."), *Path), TEXT("FRequestJournal::ReadRecordFile"));
                return false;
            }
            OutRecord.Result = *Result;
        }
        const TSharedPtr<FJsonObject>* ErrorJson = nullptr;
        if (Json->HasField(TEXT("error")))
        {
            if (!Json->TryGetObjectField(TEXT("error"), ErrorJson) || ErrorJson == nullptr)
            {
                OutError = JournalError(EErrorCode::JournalCorrupt,
                    FString::Printf(TEXT("Request journal '%s' has an invalid error object."), *Path), TEXT("FRequestJournal::ReadRecordFile"));
                return false;
            }
            FProtocolError Error;
            if (!ParseError(*ErrorJson, Error))
            {
                OutError = JournalError(EErrorCode::JournalCorrupt,
                    FString::Printf(TEXT("Request journal '%s' has an invalid error object."), *Path), TEXT("FRequestJournal::ReadRecordFile"));
                return false;
            }
            OutRecord.Error = Error;
        }
        if (OutRecord.State != ERequestJournalState::Terminal
            && (OutRecord.bInterrupted || OutRecord.bRecoveryRequired || OutRecord.Result.IsValid() || OutRecord.Error.IsSet()))
        {
            OutError = JournalError(EErrorCode::JournalCorrupt,
                FString::Printf(TEXT("Request journal '%s' has terminal data in a non-terminal state."), *Path), TEXT("FRequestJournal::ReadRecordFile"));
            return false;
        }
        if (OutRecord.State == ERequestJournalState::Terminal
            && OutRecord.TerminalPhase == EJobPhase::Failed && !OutRecord.Error.IsSet())
        {
            OutError = JournalError(EErrorCode::JournalCorrupt,
                FString::Printf(TEXT("Request journal '%s' has a failed terminal phase without an error."), *Path), TEXT("FRequestJournal::ReadRecordFile"));
            return false;
        }
        if (OutRecord.State == ERequestJournalState::Terminal
            && OutRecord.TerminalPhase != EJobPhase::Failed && OutRecord.Error.IsSet())
        {
            OutError = JournalError(EErrorCode::JournalCorrupt,
                FString::Printf(TEXT("Request journal '%s' has an error for a non-failed terminal phase."), *Path), TEXT("FRequestJournal::ReadRecordFile"));
            return false;
        }
        if (OutRecord.bInterrupted
            && (!OutRecord.bRecoveryRequired || OutRecord.TerminalPhase != EJobPhase::Failed || !OutRecord.Error.IsSet()
                || OutRecord.Error.GetValue().Code != EErrorCode::RequestInterrupted))
        {
            OutError = JournalError(EErrorCode::JournalCorrupt,
                FString::Printf(TEXT("Request journal '%s' has inconsistent interruption metadata."), *Path), TEXT("FRequestJournal::ReadRecordFile"));
            return false;
        }
        return true;
    }

    bool FRequestJournal::WriteRecordFile(const FRequestJournalRecord& Record, FProtocolError& OutError) const
    {
        const FString Destination = RecordPath(Record.RequestId);
        const FString Temporary = Destination + TEXT(".") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".tmp");
        if (!FFileHelper::SaveStringToFile(SerializeObject(Record.ToJson()), *Temporary, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
        {
            IFileManager::Get().Delete(*Temporary, false, true, true);
            OutError = JournalError(EErrorCode::JournalIoError,
                FString::Printf(TEXT("Cannot write temporary request journal '%s'."), *Temporary), TEXT("FRequestJournal::WriteRecordFile"));
            return false;
        }
        if (!IFileManager::Get().Move(*Destination, *Temporary, true, true, false, true))
        {
            IFileManager::Get().Delete(*Temporary, false, true, true);
            OutError = JournalError(EErrorCode::JournalIoError,
                FString::Printf(TEXT("Cannot atomically replace request journal '%s'."), *Destination), TEXT("FRequestJournal::WriteRecordFile"));
            return false;
        }
        OutError = FProtocolError();
        return true;
    }

    bool FRequestJournal::EnsureReady(FProtocolError& OutError) const
    {
        if (!bInitialized)
        {
            OutError = JournalError(EErrorCode::JournalIoError, TEXT("Request journal is not initialized."), TEXT("FRequestJournal::EnsureReady"));
            return false;
        }
        if (InitializationError.IsSet())
        {
            OutError = InitializationError.GetValue();
            return false;
        }
        OutError = FProtocolError();
        return true;
    }

    bool FRequestJournal::LockAcrossProcesses(const TCHAR* Callsite, FProtocolError& OutError) const
    {
        if (InterprocessMutexHandle == nullptr)
        {
            OutError = JournalError(EErrorCode::JournalIoError,
                InterprocessMutexInitializationError.IsEmpty()
                    ? FString::Printf(TEXT("Request journal cross-process mutex '%s' is unavailable."), *InterprocessMutexName)
                    : InterprocessMutexInitializationError,
                Callsite);
            return false;
        }
#if PLATFORM_WINDOWS
        const uint32 WaitResult = WaitForSingleObject(static_cast<HANDLE>(InterprocessMutexHandle), 30000);
        if (WaitResult == WAIT_OBJECT_0 || WaitResult == WAIT_ABANDONED)
        {
            OutError = FProtocolError();
            return true;
        }
        const uint32 WindowsError = WaitResult == WAIT_FAILED ? static_cast<uint32>(GetLastError()) : 0;
        OutError = JournalError(EErrorCode::JournalIoError,
            FString::Printf(TEXT("Cannot acquire request journal cross-process mutex '%s' (waitResult=%lu, windowsError=%lu)."),
                *InterprocessMutexName, WaitResult, WindowsError),
            Callsite);
        return false;
#else
        OutError = JournalError(EErrorCode::JournalIoError,
            InterprocessMutexInitializationError, Callsite);
        return false;
#endif
    }

    void FRequestJournal::UnlockAcrossProcesses() const
    {
#if PLATFORM_WINDOWS
        if (InterprocessMutexHandle != nullptr && !ReleaseMutex(static_cast<HANDLE>(InterprocessMutexHandle)))
        {
            UE_LOG(LogTemp, Error, TEXT("Request journal failed to release cross-process mutex '%s' (Windows error %lu)."),
                *InterprocessMutexName, static_cast<uint32>(GetLastError()));
        }
#endif
    }

    void FRequestJournal::CacheRecord(const FRequestJournalRecord& Record) const
    {
        Cache.Add(Record.RequestId, Record);
        CacheOrder.Remove(Record.RequestId);
        CacheOrder.Add(Record.RequestId);
        while (CacheOrder.Num() > MaxCacheEntries)
        {
            Cache.Remove(CacheOrder[0]);
            CacheOrder.RemoveAt(0);
        }
    }
}
