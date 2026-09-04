#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace CodexUnrealBlueprint
{
    static const TCHAR* const PluginVersion = TEXT("1.0.0");
    static const TCHAR* const ProtocolVersion = TEXT("2.0.0");

    enum class EServiceState : uint8
    {
        Stopped,
        Starting,
        Unavailable,
        Listening,
        Connected,
        Busy,
        Faulted
    };

    enum class EJobPhase : uint8
    {
        Queued,
        Preflight,
        Modify,
        Compile,
        Save,
        Reload,
        Verify,
        Stopping,
        Succeeded,
        Failed,
        Cancelled
    };

    enum class EErrorCode : uint8
    {
        None,
        InvalidJson,
        InvalidRequest,
        ProtocolVersionMismatch,
        RequestIdRequired,
        AuthenticationRequired,
        AuthenticationFailed,
        TransportError,
        RequestConflict,
        RequestNotFound,
        RequestInterrupted,
        JournalCorrupt,
        JournalIoError,
        InvalidArgument,
        UnknownField,
        UnknownOperation,
        TypeMismatch,
        AssetNotFound,
        ValidationFailed,
        VerificationFailed,
        NotImplemented,
        JobQueueFull,
        WriteLeaseExpired,
        TransportQueueFull,
        InternalError
    };

    CODEXUNREALBLUEPRINTCORE_API const TCHAR* LexToString(EServiceState State);
    CODEXUNREALBLUEPRINTCORE_API const TCHAR* LexToString(EJobPhase Phase);
    CODEXUNREALBLUEPRINTCORE_API const TCHAR* LexToString(EErrorCode Code);

    struct CODEXUNREALBLUEPRINTCORE_API FProtocolError
    {
        EErrorCode Code = EErrorCode::None;
        FString Message;
        FString AssetPath;
        int32 OperationIndex = INDEX_NONE;
        FString UECallsite;
        TArray<FString> CompilerMessages;

        static FProtocolError Make(EErrorCode InCode, const FString& InMessage, const FString& InCallsite);
        TSharedRef<FJsonObject> ToJson() const;
    };

    struct CODEXUNREALBLUEPRINTCORE_API FProtocolRequest
    {
        FString JsonRpc;
        FString Id;
        TSharedPtr<FJsonValue> IdJsonValue;
        FString Method;
        TSharedPtr<FJsonObject> Params;

        static bool Parse(const FString& Json, FProtocolRequest& OutRequest, FProtocolError& OutError);
    };

    struct CODEXUNREALBLUEPRINTCORE_API FProtocolResponse
    {
        FString Id;
        TSharedPtr<FJsonValue> IdJsonValue;
        TSharedPtr<FJsonObject> Result;
        TOptional<FProtocolError> Error;

        bool IsSuccess() const;
        FString ToJsonString() const;
    };
}
