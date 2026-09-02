#include "PiUnrealBlueprintProtocol.h"

#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace PiUnrealBlueprint
{
    const TCHAR* LexToString(const EServiceState State)
    {
        switch (State)
        {
        case EServiceState::Stopped: return TEXT("Stopped");
        case EServiceState::Starting: return TEXT("Starting");
        case EServiceState::Unavailable: return TEXT("Unavailable");
        case EServiceState::Listening: return TEXT("Listening");
        case EServiceState::Connected: return TEXT("Connected");
        case EServiceState::Busy: return TEXT("Busy");
        case EServiceState::Faulted: return TEXT("Faulted");
        default: return TEXT("Unknown");
        }
    }

    const TCHAR* LexToString(const EJobPhase Phase)
    {
        switch (Phase)
        {
        case EJobPhase::Queued: return TEXT("Queued");
        case EJobPhase::Preflight: return TEXT("Preflight");
        case EJobPhase::Backup: return TEXT("Backup");
        case EJobPhase::Modify: return TEXT("Modify");
        case EJobPhase::Compile: return TEXT("Compile");
        case EJobPhase::Save: return TEXT("Save");
        case EJobPhase::Reload: return TEXT("Reload");
        case EJobPhase::Verify: return TEXT("Verify");
        case EJobPhase::Recover: return TEXT("Recover");
        case EJobPhase::Succeeded: return TEXT("Succeeded");
        case EJobPhase::Failed: return TEXT("Failed");
        case EJobPhase::Cancelled: return TEXT("Cancelled");
        default: return TEXT("Unknown");
        }
    }

    const TCHAR* LexToString(const EErrorCode Code)
    {
        switch (Code)
        {
        case EErrorCode::None: return TEXT("None");
        case EErrorCode::InvalidJson: return TEXT("InvalidJson");
        case EErrorCode::InvalidRequest: return TEXT("InvalidRequest");
        case EErrorCode::ProtocolVersionMismatch: return TEXT("ProtocolVersionMismatch");
        case EErrorCode::RequestIdRequired: return TEXT("RequestIdRequired");
        case EErrorCode::AuthenticationRequired: return TEXT("AuthenticationRequired");
        case EErrorCode::AuthenticationFailed: return TEXT("AuthenticationFailed");
        case EErrorCode::TransportError: return TEXT("TransportError");
        case EErrorCode::NotImplemented: return TEXT("NotImplemented");
        case EErrorCode::InternalError: return TEXT("InternalError");
        default: return TEXT("Unknown");
        }
    }

    FProtocolError FProtocolError::Make(const EErrorCode InCode, const FString& InMessage, const FString& InCallsite)
    {
        FProtocolError Error;
        Error.Code = InCode;
        Error.Message = InMessage;
        Error.UECallsite = InCallsite;
        return Error;
    }

    TSharedRef<FJsonObject> FProtocolError::ToJson() const
    {
        TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("code"), LexToString(Code));
        Data->SetStringField(TEXT("message"), Message);
        Data->SetStringField(TEXT("ueCallsite"), UECallsite);
        if (!AssetPath.IsEmpty())
        {
            Data->SetStringField(TEXT("assetPath"), AssetPath);
        }
        if (OperationIndex != INDEX_NONE)
        {
            Data->SetNumberField(TEXT("operationIndex"), OperationIndex);
        }
        if (CompilerMessages.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Messages;
            for (const FString& CompilerMessage : CompilerMessages)
            {
                Messages.Add(MakeShared<FJsonValueString>(CompilerMessage));
            }
            Data->SetArrayField(TEXT("compilerMessages"), Messages);
        }
        return Data;
    }

    bool FProtocolRequest::Parse(const FString& Json, FProtocolRequest& OutRequest, FProtocolError& OutError)
    {
        TSharedPtr<FJsonObject> Root;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
        if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
        {
            OutError = FProtocolError::Make(EErrorCode::InvalidJson, TEXT("Request file is not a valid JSON object."), TEXT("FProtocolRequest::Parse"));
            return false;
        }

        static const TSet<FString> AllowedFields = { TEXT("jsonrpc"), TEXT("id"), TEXT("method"), TEXT("params") };
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Root->Values)
        {
            if (!AllowedFields.Contains(Field.Key))
            {
                OutError = FProtocolError::Make(EErrorCode::InvalidRequest,
                    FString::Printf(TEXT("Unknown request field '%s'."), *Field.Key), TEXT("FProtocolRequest::Parse"));
                return false;
            }
        }

        if (!Root->TryGetStringField(TEXT("jsonrpc"), OutRequest.JsonRpc) || OutRequest.JsonRpc != TEXT("2.0"))
        {
            OutError = FProtocolError::Make(EErrorCode::ProtocolVersionMismatch,
                TEXT("Field 'jsonrpc' must be the string '2.0'."), TEXT("FProtocolRequest::Parse"));
            return false;
        }
        OutRequest.IdJsonValue = Root->TryGetField(TEXT("id"));
        if (!OutRequest.IdJsonValue.IsValid())
        {
            OutError = FProtocolError::Make(EErrorCode::RequestIdRequired,
                TEXT("A string or number JSON-RPC id is required."), TEXT("FProtocolRequest::Parse"));
            return false;
        }
        if (OutRequest.IdJsonValue->Type == EJson::String)
        {
            OutRequest.Id = OutRequest.IdJsonValue->AsString();
            if (OutRequest.Id.IsEmpty())
            {
                OutError = FProtocolError::Make(EErrorCode::RequestIdRequired,
                    TEXT("A string JSON-RPC id must not be empty."), TEXT("FProtocolRequest::Parse"));
                return false;
            }
        }
        else if (OutRequest.IdJsonValue->Type == EJson::Number)
        {
            const double NumberId = OutRequest.IdJsonValue->AsNumber();
            if (!FMath::IsFinite(NumberId))
            {
                OutError = FProtocolError::Make(EErrorCode::RequestIdRequired,
                    TEXT("A numeric JSON-RPC id must be finite."), TEXT("FProtocolRequest::Parse"));
                return false;
            }
            OutRequest.Id = FString::SanitizeFloat(NumberId);
        }
        else
        {
            OutError = FProtocolError::Make(EErrorCode::RequestIdRequired,
                TEXT("A string or number JSON-RPC id is required."), TEXT("FProtocolRequest::Parse"));
            return false;
        }
        if (!Root->TryGetStringField(TEXT("method"), OutRequest.Method) || OutRequest.Method.IsEmpty())
        {
            OutError = FProtocolError::Make(EErrorCode::InvalidRequest,
                TEXT("A non-empty string method is required."), TEXT("FProtocolRequest::Parse"));
            return false;
        }

        if (Root->HasField(TEXT("params")))
        {
            const TSharedPtr<FJsonObject>* ParamsObject = nullptr;
            if (!Root->TryGetObjectField(TEXT("params"), ParamsObject) || ParamsObject == nullptr)
            {
                OutError = FProtocolError::Make(EErrorCode::InvalidRequest,
                    TEXT("Field 'params' must be an object when present."), TEXT("FProtocolRequest::Parse"));
                return false;
            }
            OutRequest.Params = *ParamsObject;
        }
        else
        {
            OutRequest.Params = MakeShared<FJsonObject>();
        }
        return true;
    }

    bool FProtocolResponse::IsSuccess() const
    {
        return Result.IsValid() && !Error.IsSet();
    }

    FString FProtocolResponse::ToJsonString() const
    {
        TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
        if (IdJsonValue.IsValid())
        {
            Root->SetField(TEXT("id"), IdJsonValue);
        }
        else if (Id.IsEmpty())
        {
            Root->SetField(TEXT("id"), MakeShared<FJsonValueNull>());
        }
        else
        {
            Root->SetStringField(TEXT("id"), Id);
        }

        if (Error.IsSet())
        {
            const FProtocolError& ErrorValue = Error.GetValue();
            TSharedRef<FJsonObject> JsonRpcError = MakeShared<FJsonObject>();
            int32 JsonRpcCode = -32600;
            switch (ErrorValue.Code)
            {
            case EErrorCode::InvalidJson: JsonRpcCode = -32700; break;
            case EErrorCode::NotImplemented: JsonRpcCode = -32601; break;
            case EErrorCode::AuthenticationRequired: JsonRpcCode = -32001; break;
            case EErrorCode::AuthenticationFailed: JsonRpcCode = -32002; break;
            case EErrorCode::ProtocolVersionMismatch: JsonRpcCode = -32003; break;
            case EErrorCode::TransportError: JsonRpcCode = -32004; break;
            default: break;
            }
            JsonRpcError->SetNumberField(TEXT("code"), JsonRpcCode);
            JsonRpcError->SetStringField(TEXT("message"), ErrorValue.Message);
            JsonRpcError->SetObjectField(TEXT("data"), ErrorValue.ToJson());
            Root->SetObjectField(TEXT("error"), JsonRpcError);
        }
        else
        {
            Root->SetObjectField(TEXT("result"), Result.IsValid() ? Result.ToSharedRef() : MakeShared<FJsonObject>());
        }

        FString Output;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
        FJsonSerializer::Serialize(Root, Writer);
        return Output;
    }
}
