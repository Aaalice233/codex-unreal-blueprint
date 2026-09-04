#include "CodexUnrealBlueprintService.h"

#include "CodexUnrealBlueprintJobs.h"
#include "CodexUnrealBlueprintEditorSafeDispatcher.h"
#include "CodexUnrealBlueprintInspection.h"
#include "CodexUnrealAssetInspection.h"
#include "CodexUnrealBlueprintOperationRegistry.h"
#include "CodexUnrealBlueprintPreflight.h"
#include "CodexUnrealBlueprintRequestJournal.h"
#include "CodexUnrealBlueprintRuntimeStatus.h"
#include "CodexUnrealBlueprintSearch.h"
#include "CodexUnrealBlueprintSourceControl.h"
#include "CodexUnrealBlueprintVerification.h"
#include "CodexUnrealBlueprintWritePipeline.h"

#include "Editor.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "UObject/UObjectIterator.h"

namespace CodexUnrealBlueprint
{
    namespace
    {
        FProtocolResponse InvalidJobRequest(const FProtocolRequest& Request, const FString& Message, const FString& Callsite)
        {
            FProtocolResponse Response;
            Response.Id = Request.Id;
            Response.IdJsonValue = Request.IdJsonValue;
            Response.Error = FProtocolError::Make(EErrorCode::InvalidRequest, Message, Callsite);
            return Response;
        }

        bool ResolveJob(const FProtocolRequest& Request, FJobSnapshot& OutSnapshot)
        {
            if (!Request.Params.IsValid()) return false;
            FString JobId;
            if (Request.Params->TryGetStringField(TEXT("jobId"), JobId) && !JobId.IsEmpty())
            {
                return FJobManager::Get().Get(JobId, OutSnapshot);
            }
            FString RequestId;
            return Request.Params->TryGetStringField(TEXT("requestId"), RequestId)
                && !RequestId.IsEmpty()
                && FJobManager::Get().FindByRequestId(RequestId, OutSnapshot);
        }

        bool IsWriteRequest(const FProtocolRequest& Request)
        {
            return Request.Method == TEXT("blueprint.apply");
        }

        FProtocolResponse RequestIdRequired(const FProtocolRequest& Request)
        {
            FProtocolResponse Response;
            Response.Id = Request.Id;
            Response.IdJsonValue = Request.IdJsonValue;
            Response.Error = FProtocolError::Make(EErrorCode::RequestIdRequired,
                TEXT("This job request requires a non-empty requestId in params."), TEXT("FCoreService::Dispatch"));
            return Response;
        }

        FProtocolResponse ErrorResponse(const FProtocolRequest& Request, const FProtocolError& Error)
        {
            FProtocolResponse Response; Response.Id = Request.Id; Response.IdJsonValue = Request.IdJsonValue; Response.Error = Error; return Response;
        }

        bool RejectUnknownParams(const FProtocolRequest& Request, const TSet<FString>& Allowed, FProtocolResponse& OutResponse)
        {
            if (!Request.Params.IsValid()) return true;
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Request.Params->Values)
            {
                if (Allowed.Contains(Pair.Key)) continue;
                OutResponse = ErrorResponse(Request, FProtocolError::Make(EErrorCode::UnknownField,
                    FString::Printf(TEXT("Method '%s' rejects unknown field '%s'."), *Request.Method, *Pair.Key),
                    TEXT("FCoreService::RejectUnknownParams")));
                return false;
            }
            return true;
        }

        bool ReadOperations(const FProtocolRequest& Request, TArray<TSharedRef<FJsonObject>>& OutOperations, FProtocolResponse& OutResponse)
        {
            const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
            if (!Request.Params.IsValid() || !Request.Params->TryGetArrayField(TEXT("operations"), Values)
                || !Values || Values->Num() == 0 || Values->Num() > 500)
            {
                OutResponse = ErrorResponse(Request, FProtocolError::Make(EErrorCode::InvalidArgument,
                    TEXT("operations must be an array containing 1 to 500 objects."), TEXT("FCoreService::ReadOperations")));
                return false;
            }
            for (const TSharedPtr<FJsonValue>& Value : *Values)
            {
                if (!Value.IsValid() || Value->Type != EJson::Object)
                {
                    OutResponse = ErrorResponse(Request, FProtocolError::Make(EErrorCode::TypeMismatch,
                        TEXT("Every operations item must be an object."), TEXT("FCoreService::ReadOperations")));
                    return false;
                }
                OutOperations.Add(Value->AsObject().ToSharedRef());
            }
            return true;
        }

        bool ReadExpectedHashes(const FProtocolRequest& Request, TMap<FString, FString>& OutHashes, FProtocolResponse& OutResponse)
        {
            if (!Request.Params->HasField(TEXT("expectedStructureHashes"))) return true;
            const TSharedPtr<FJsonObject>* Json = nullptr;
            if (!Request.Params->TryGetObjectField(TEXT("expectedStructureHashes"), Json) || !Json)
            {
                OutResponse = ErrorResponse(Request, FProtocolError::Make(EErrorCode::TypeMismatch,
                    TEXT("expectedStructureHashes must be an object of package-name to hash strings."), TEXT("FCoreService::ReadExpectedHashes")));
                return false;
            }
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Json)->Values)
            {
                FString Hash;
                if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(Hash) || Hash.IsEmpty())
                {
                    OutResponse = ErrorResponse(Request, FProtocolError::Make(EErrorCode::TypeMismatch,
                        TEXT("Every expectedStructureHashes value must be a non-empty string."), TEXT("FCoreService::ReadExpectedHashes")));
                    return false;
                }
                OutHashes.Add(Pair.Key, Hash);
            }
            return true;
        }

        EJobPhase PhaseFromString(const FString& Phase)
        {
            if (Phase == TEXT("modify")) return EJobPhase::Modify;
            if (Phase == TEXT("compile")) return EJobPhase::Compile;
            if (Phase == TEXT("save")) return EJobPhase::Save;
            if (Phase == TEXT("reload")) return EJobPhase::Reload;
            if (Phase == TEXT("verify")) return EJobPhase::Verify;
            return EJobPhase::Preflight;
        }

        bool ParseOffsetCursor(const FString& Cursor, int32& OutOffset)
        {
            OutOffset = 0;
            if (Cursor.IsEmpty()) return true;
            for (const TCHAR Character : Cursor) if (!FChar::IsDigit(Character)) return false;
            const int64 Value = FCString::Atoi64(*Cursor);
            if (Value < 0 || Value > MAX_int32) return false;
            OutOffset = static_cast<int32>(Value);
            return true;
        }

        FProtocolResponse RunOnGameThread(TFunction<FProtocolResponse()> Work)
        {
            if (IsInGameThread()) return Work();
            FProtocolResponse Response; FEvent* Event = FPlatformProcess::GetSynchEventFromPool(true);
            const bool bQueued = FEditorSafeDispatcher::Get().Enqueue(
                [&Response, &Work, Event]() { Response = Work(); Event->Trigger(); },
                [&Response, Event]()
                {
                    Response.Error = FProtocolError::Make(EErrorCode::InternalError,
                        TEXT("The Editor-safe dispatcher stopped before the request could run."),
                        TEXT("FEditorSafeDispatcher::Shutdown"));
                    Event->Trigger();
                });
            if (!bQueued)
            {
                Response.Error = FProtocolError::Make(EErrorCode::InternalError,
                    TEXT("The Editor-safe dispatcher is not accepting requests."),
                    TEXT("FEditorSafeDispatcher::Enqueue"));
                FPlatformProcess::ReturnSynchEventToPool(Event);
                return Response;
            }
            Event->Wait(); FPlatformProcess::ReturnSynchEventToPool(Event); return Response;
        }
    }

    FCoreService& FCoreService::Get()
    {
        static FCoreService Instance;
        return Instance;
    }

    FProtocolResponse FCoreService::Dispatch(const FProtocolRequest& Request) const
    {
        if (IsWriteRequest(Request))
        {
            FString RequestId;
            if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("requestId"), RequestId)
                || RequestId.TrimStartAndEnd().IsEmpty()) return RequestIdRequired(Request);
        }
        if (Request.Method == TEXT("unreal.status") || Request.Method == TEXT("unreal_status")) return RunOnGameThread([this, &Request]() { return GetStatus(Request); });
        if (Request.Method == TEXT("unreal.doctor")) return RunOnGameThread([this, &Request]() { return Doctor(Request); });
        if (Request.Method == TEXT("unreal.search")) return RunOnGameThread([this, &Request]() { return Search(Request); });
        if (Request.Method == TEXT("unreal.asset.inspect")) return RunOnGameThread([this, &Request]() { return InspectAsset(Request); });
        if (Request.Method == TEXT("unreal.asset.compare")) return RunOnGameThread([this, &Request]() { return CompareAssets(Request); });
        if (Request.Method == TEXT("unreal.asset.referencers")) return RunOnGameThread([this, &Request]() { return FindAssetReferencers(Request); });
        if (Request.Method == TEXT("blueprint.capabilities")) return Capabilities(Request);
        if (Request.Method == TEXT("blueprint.inspect")) return RunOnGameThread([this, &Request]() { return Inspect(Request); });
        if (Request.Method == TEXT("blueprint.validate")) return Validate(Request);
        if (Request.Method == TEXT("blueprint.apply")) return Apply(Request);
        if (Request.Method == TEXT("blueprint.verify")) return Verify(Request);
        if (Request.Method == TEXT("blueprint.request")) return GetRequestJournal(Request);
        if (Request.Method == TEXT("blueprint.job"))
        {
            FString Action;
            if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("action"), Action))
                return InvalidJobRequest(Request, TEXT("blueprint.job requires action query, wait, or cancel."), TEXT("FCoreService::Dispatch"));
            if (Action == TEXT("query")) return GetJob(Request);
            if (Action == TEXT("cancel")) return CancelJob(Request);
            if (Action == TEXT("wait")) return InvalidJobRequest(Request, TEXT("blueprint.job wait must use asynchronous dispatch."), TEXT("FCoreService::Dispatch"));
            return InvalidJobRequest(Request, TEXT("Unknown blueprint.job action."), TEXT("FCoreService::Dispatch"));
        }
        FProtocolResponse Response = MakeNotImplemented(Request.Id, Request.Method);
        Response.IdJsonValue = Request.IdJsonValue;
        return Response;
    }

    void FCoreService::DispatchAsync(const FProtocolRequest& Request, FResponseCallback Callback) const
    {
        FString Action;
        if (Request.Method != TEXT("blueprint.job") || !Request.Params.IsValid()
            || !Request.Params->TryGetStringField(TEXT("action"), Action) || Action != TEXT("wait"))
        {
            Callback(Dispatch(Request));
            return;
        }
        FProtocolResponse Strict;
        if (!RejectUnknownParams(Request, {TEXT("action"),TEXT("jobId"),TEXT("requestId"),TEXT("timeoutMs")}, Strict))
        {
            Callback(MoveTemp(Strict));
            return;
        }

        FJobSnapshot Existing;
        if (!ResolveJob(Request, Existing))
        {
            Callback(InvalidJobRequest(Request, TEXT("Unknown jobId or requestId."), TEXT("FCoreService::DispatchAsync")));
            return;
        }
        double TimeoutMs = 30000.0;
        if (Request.Params->HasField(TEXT("timeoutMs"))
            && (!Request.Params->TryGetNumberField(TEXT("timeoutMs"), TimeoutMs) || TimeoutMs < 0.0 || TimeoutMs > 600000.0))
        {
            Callback(InvalidJobRequest(Request, TEXT("timeoutMs must be between 0 and 600000."), TEXT("FCoreService::DispatchAsync")));
            return;
        }
        const FString ResponseId = Request.Id;
        const TSharedPtr<FJsonValue> ResponseIdJsonValue = Request.IdJsonValue;
        FJobManager::Get().Wait(Existing.JobId, TimeoutMs / 1000.0,
            [Callback = MoveTemp(Callback), ResponseId, ResponseIdJsonValue](const FJobSnapshot& Snapshot) mutable
            {
                FProtocolResponse Response;
                Response.Id = ResponseId;
                Response.IdJsonValue = ResponseIdJsonValue;
                Response.Result = Snapshot.ToJson();
                Callback(MoveTemp(Response));
            });
    }

    FProtocolResponse FCoreService::MakeNotImplemented(const FString& RequestId, const FString& Method) const
    {
        FProtocolResponse Response;
        Response.Id = RequestId;
        Response.Error = FProtocolError::Make(EErrorCode::NotImplemented,
            FString::Printf(TEXT("Method '%s' is not implemented in plugin %s."), *Method, PluginVersion),
            TEXT("FCoreService::Dispatch"));
        return Response;
    }

    FProtocolResponse FCoreService::GetStatus(const FProtocolRequest& Request) const
    {
        FProtocolResponse Response;
        Response.Id = Request.Id;
        Response.IdJsonValue = Request.IdJsonValue;
        Response.Result = MakeShared<FJsonObject>();
        Response.Result->SetStringField(TEXT("pluginVersion"), PluginVersion);
        Response.Result->SetStringField(TEXT("protocolVersion"), ProtocolVersion);
        FProtocolResponse Strict;
        if (!RejectUnknownParams(Request, {}, Strict)) return Strict;
        const FRuntimeTransportStatus TransportStatus = FRuntimeStatusRegistry::Get().GetTransportStatus();
        Response.Result->SetStringField(TEXT("serviceState"), LexToString(TransportStatus.State));
        Response.Result->SetBoolField(TEXT("coreAvailable"), true);
        Response.Result->SetBoolField(TEXT("transportAvailable"), TransportStatus.bAvailable);
        if (TransportStatus.Error.Code != EErrorCode::None)
        {
            Response.Result->SetObjectField(TEXT("transportError"), TransportStatus.Error.ToJson());
        }
        Response.Result->SetBoolField(TEXT("pie"), GEditor && GEditor->PlayWorld != nullptr);
        TArray<TSharedPtr<FJsonValue>> DirtyPackages;
        for (TObjectIterator<UPackage> It; It; ++It) if (It->IsDirty() && !It->HasAnyPackageFlags(PKG_CompiledIn)) DirtyPackages.Add(MakeShared<FJsonValueString>(It->GetName()));
        Response.Result->SetArrayField(TEXT("dirtyPackages"), DirtyPackages);
        Response.Result->SetObjectField(TEXT("jobQueue"), FJobManager::Get().GetStatus().ToJson());
        TArray<TSharedPtr<FJsonValue>> Methods;
        const TCHAR* Implemented[] = {TEXT("unreal.status"),TEXT("unreal.doctor"),TEXT("unreal.search"),TEXT("unreal.asset.inspect"),TEXT("unreal.asset.compare"),TEXT("unreal.asset.referencers"),TEXT("blueprint.capabilities"),TEXT("blueprint.inspect"),TEXT("blueprint.validate"),TEXT("blueprint.apply"),TEXT("blueprint.job"),TEXT("blueprint.verify")};
        for (const TCHAR* Method : Implemented) Methods.Add(MakeShared<FJsonValueString>(Method));
        FRequestJournalStatus JournalStatus;
        FProtocolError JournalError;
        if (FRequestJournal::Get().GetStatus(JournalStatus, JournalError))
            Response.Result->SetObjectField(TEXT("requestJournal"), JournalStatus.ToJson());
        else
        {
            JournalStatus.bHealthy = false;
            JournalStatus.Error = JournalError;
            Response.Result->SetObjectField(TEXT("requestJournal"), JournalStatus.ToJson());
        }
        Response.Result->SetArrayField(TEXT("implementedMethods"), Methods);
        return Response;
    }

    FProtocolResponse FCoreService::Doctor(const FProtocolRequest& Request) const
    {
        FProtocolResponse Strict; if (!RejectUnknownParams(Request, {}, Strict)) return Strict;
        FProtocolResponse Response; Response.Id = Request.Id; Response.IdJsonValue = Request.IdJsonValue; Response.Result = MakeShared<FJsonObject>();
        FRequestJournalStatus Journal; FProtocolError JournalError; const bool bJournal = FRequestJournal::Get().GetStatus(Journal, JournalError);
        FSourceControlResult SourceControl = FWriteSourceControl::Inspect({}, {});
        Response.Result->SetBoolField(TEXT("healthy"), bJournal && SourceControl.bSucceeded);
        Response.Result->SetStringField(TEXT("pluginVersion"), PluginVersion); Response.Result->SetStringField(TEXT("protocolVersion"), ProtocolVersion);
        Response.Result->SetStringField(TEXT("projectDir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
        Response.Result->SetObjectField(TEXT("requestJournal"), Journal.ToJson());
        TSharedRef<FJsonObject> SourceControlJson = MakeShared<FJsonObject>();
        SourceControlJson->SetBoolField(TEXT("healthy"), SourceControl.bSucceeded);
        TSharedRef<FJsonObject> EditorProvider = MakeShared<FJsonObject>();
        EditorProvider->SetBoolField(TEXT("enabled"), SourceControl.bProviderEnabled);
        EditorProvider->SetBoolField(TEXT("available"), SourceControl.bProviderAvailable);
        EditorProvider->SetStringField(TEXT("name"), SourceControl.ProviderName);
        SourceControlJson->SetObjectField(TEXT("editorProvider"), EditorProvider);
        TSharedRef<FJsonObject> WorkingCopy = MakeShared<FJsonObject>();
        WorkingCopy->SetStringField(TEXT("kind"), SourceControl.WorkingCopyKind);
        WorkingCopy->SetStringField(TEXT("root"), SourceControl.WorkingCopyRoot);
        SourceControlJson->SetObjectField(TEXT("workingCopy"), WorkingCopy);
        if (!SourceControl.Error.IsEmpty()) SourceControlJson->SetStringField(TEXT("error"), SourceControl.Error);
        Response.Result->SetObjectField(TEXT("sourceControl"), SourceControlJson);
        if (!bJournal) Response.Result->SetObjectField(TEXT("journalError"), JournalError.ToJson());
        return Response;
    }

    FProtocolResponse FCoreService::Search(const FProtocolRequest& Request) const
    {
        FProtocolResponse Strict; if (!RejectUnknownParams(Request, {TEXT("query"),TEXT("domain"),TEXT("context"),TEXT("cursor"),TEXT("limit")}, Strict)) return Strict;
        FString Query, Domain(TEXT("asset")), Cursor; double LimitNumber = 50; Request.Params->TryGetStringField(TEXT("query"), Query); Request.Params->TryGetStringField(TEXT("domain"), Domain); Request.Params->TryGetStringField(TEXT("cursor"), Cursor); Request.Params->TryGetNumberField(TEXT("limit"), LimitNumber);
        if (Query.TrimStartAndEnd().IsEmpty() || LimitNumber < 1 || LimitNumber > 200) return ErrorResponse(Request, FProtocolError::Make(EErrorCode::InvalidArgument, TEXT("query is required and limit must be between 1 and 200."), TEXT("FCoreService::Search")));
        const TSharedPtr<FJsonObject>* Context = nullptr; if (Request.Params->HasField(TEXT("context")) && !Request.Params->TryGetObjectField(TEXT("context"), Context)) return ErrorResponse(Request, FProtocolError::Make(EErrorCode::TypeMismatch, TEXT("context must be an object."), TEXT("FCoreService::Search")));
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>(); FProtocolError Error;
        if (!FBlueprintSearch::Search(Query, Domain, Context ? *Context : TSharedPtr<FJsonObject>(), Cursor, static_cast<int32>(LimitNumber), Result, Error)) return ErrorResponse(Request, Error);
        FProtocolResponse Response; Response.Id = Request.Id; Response.IdJsonValue = Request.IdJsonValue; Response.Result = Result; return Response;
    }

    FProtocolResponse FCoreService::InspectAsset(const FProtocolRequest& Request) const
    {
        FProtocolResponse Strict;
        if (!RejectUnknownParams(Request, {TEXT("assetPath"), TEXT("facets"), TEXT("propertyPaths"), TEXT("cursor"), TEXT("limit")}, Strict)) return Strict;
        FString AssetPath, Cursor; double LimitNumber = 500;
        if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("assetPath"), AssetPath)
            || AssetPath.TrimStartAndEnd().IsEmpty())
            return ErrorResponse(Request, FProtocolError::Make(EErrorCode::InvalidArgument,
                TEXT("assetPath is required."), TEXT("FCoreService::InspectAsset")));
        if ((Request.Params->HasField(TEXT("cursor")) && !Request.Params->TryGetStringField(TEXT("cursor"), Cursor))
            || (Request.Params->HasField(TEXT("limit")) && !Request.Params->TryGetNumberField(TEXT("limit"), LimitNumber)))
            return ErrorResponse(Request, FProtocolError::Make(EErrorCode::TypeMismatch,
                TEXT("cursor must be a string and limit must be a number."), TEXT("FCoreService::InspectAsset")));
        int32 Offset = 0;
        if (!ParseOffsetCursor(Cursor, Offset) || !FMath::IsNearlyEqual(LimitNumber, FMath::RoundToDouble(LimitNumber))
            || LimitNumber < 1 || LimitNumber > 500)
            return ErrorResponse(Request, FProtocolError::Make(EErrorCode::InvalidArgument,
                TEXT("cursor must be a non-negative decimal offset and limit must be an integer from 1 to 500."),
                TEXT("FCoreService::InspectAsset")));

        TArray<FString> Facets; TArray<FString> PropertyPaths;
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (Request.Params->HasField(TEXT("facets")))
        {
            if (!Request.Params->TryGetArrayField(TEXT("facets"), Values) || !Values || Values->Num() > 16)
                return ErrorResponse(Request, FProtocolError::Make(EErrorCode::TypeMismatch,
                    TEXT("facets must be an array of at most 16 strings."), TEXT("FCoreService::InspectAsset")));
            for (const TSharedPtr<FJsonValue>& Value : *Values)
            {
                FString Item; if (!Value.IsValid() || !Value->TryGetString(Item) || Item.IsEmpty())
                    return ErrorResponse(Request, FProtocolError::Make(EErrorCode::TypeMismatch,
                        TEXT("facets must contain non-empty strings."), TEXT("FCoreService::InspectAsset")));
                Facets.AddUnique(Item);
            }
        }
        Values = nullptr;
        if (Request.Params->HasField(TEXT("propertyPaths")))
        {
            if (!Request.Params->TryGetArrayField(TEXT("propertyPaths"), Values) || !Values || Values->Num() > 500)
                return ErrorResponse(Request, FProtocolError::Make(EErrorCode::TypeMismatch,
                    TEXT("propertyPaths must be an array of at most 500 strings."), TEXT("FCoreService::InspectAsset")));
            for (const TSharedPtr<FJsonValue>& Value : *Values)
            {
                FString Item; if (!Value.IsValid() || !Value->TryGetString(Item) || Item.TrimStartAndEnd().IsEmpty())
                    return ErrorResponse(Request, FProtocolError::Make(EErrorCode::TypeMismatch,
                        TEXT("propertyPaths must contain non-empty strings."), TEXT("FCoreService::InspectAsset")));
                PropertyPaths.AddUnique(Item);
            }
        }
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>(); FProtocolError Error;
        if (!FUnrealAssetInspection::Inspect(AssetPath, Facets, PropertyPaths, Offset,
            static_cast<int32>(LimitNumber), Result, Error)) return ErrorResponse(Request, Error);
        FProtocolResponse Response; Response.Id = Request.Id; Response.IdJsonValue = Request.IdJsonValue;
        Response.Result = Result; return Response;
    }

    FProtocolResponse FCoreService::CompareAssets(const FProtocolRequest& Request) const
    {
        FProtocolResponse Strict;
        if (!RejectUnknownParams(Request, {TEXT("baseAssetPath"), TEXT("targetAssetPath"), TEXT("facets"), TEXT("propertyPaths"), TEXT("cursor"), TEXT("limit")}, Strict)) return Strict;
        FString BaseAssetPath, TargetAssetPath, Cursor; double LimitNumber = 500;
        if (!Request.Params.IsValid()
            || !Request.Params->TryGetStringField(TEXT("baseAssetPath"), BaseAssetPath) || BaseAssetPath.TrimStartAndEnd().IsEmpty()
            || !Request.Params->TryGetStringField(TEXT("targetAssetPath"), TargetAssetPath) || TargetAssetPath.TrimStartAndEnd().IsEmpty())
            return ErrorResponse(Request, FProtocolError::Make(EErrorCode::InvalidArgument,
                TEXT("baseAssetPath and targetAssetPath are required."), TEXT("FCoreService::CompareAssets")));
        if ((Request.Params->HasField(TEXT("cursor")) && !Request.Params->TryGetStringField(TEXT("cursor"), Cursor))
            || (Request.Params->HasField(TEXT("limit")) && !Request.Params->TryGetNumberField(TEXT("limit"), LimitNumber)))
            return ErrorResponse(Request, FProtocolError::Make(EErrorCode::TypeMismatch,
                TEXT("cursor must be a string and limit must be a number."), TEXT("FCoreService::CompareAssets")));
        int32 Offset = 0;
        if (!ParseOffsetCursor(Cursor, Offset) || !FMath::IsNearlyEqual(LimitNumber, FMath::RoundToDouble(LimitNumber))
            || LimitNumber < 1 || LimitNumber > 500)
            return ErrorResponse(Request, FProtocolError::Make(EErrorCode::InvalidArgument,
                TEXT("cursor must be a non-negative decimal offset and limit must be an integer from 1 to 500."),
                TEXT("FCoreService::CompareAssets")));
        TArray<FString> Facets; TArray<FString> PropertyPaths; const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (Request.Params->HasField(TEXT("facets")))
        {
            if (!Request.Params->TryGetArrayField(TEXT("facets"), Values) || !Values || Values->Num() > 16)
                return ErrorResponse(Request, FProtocolError::Make(EErrorCode::TypeMismatch, TEXT("facets must be an array."), TEXT("FCoreService::CompareAssets")));
            for (const TSharedPtr<FJsonValue>& Value : *Values) { FString Item; if (!Value.IsValid() || !Value->TryGetString(Item) || Item.IsEmpty()) return ErrorResponse(Request, FProtocolError::Make(EErrorCode::TypeMismatch, TEXT("facets must contain non-empty strings."), TEXT("FCoreService::CompareAssets"))); Facets.AddUnique(Item); }
        }
        Values = nullptr;
        if (Request.Params->HasField(TEXT("propertyPaths")))
        {
            if (!Request.Params->TryGetArrayField(TEXT("propertyPaths"), Values) || !Values || Values->Num() > 500)
                return ErrorResponse(Request, FProtocolError::Make(EErrorCode::TypeMismatch, TEXT("propertyPaths must be an array."), TEXT("FCoreService::CompareAssets")));
            for (const TSharedPtr<FJsonValue>& Value : *Values) { FString Item; if (!Value.IsValid() || !Value->TryGetString(Item) || Item.IsEmpty()) return ErrorResponse(Request, FProtocolError::Make(EErrorCode::TypeMismatch, TEXT("propertyPaths must contain non-empty strings."), TEXT("FCoreService::CompareAssets"))); PropertyPaths.AddUnique(Item); }
        }
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>(); FProtocolError Error;
        if (!FUnrealAssetInspection::Compare(BaseAssetPath, TargetAssetPath, Facets, PropertyPaths, Offset,
            static_cast<int32>(LimitNumber), Result, Error)) return ErrorResponse(Request, Error);
        FProtocolResponse Response; Response.Id = Request.Id; Response.IdJsonValue = Request.IdJsonValue;
        Response.Result = Result; return Response;
    }

    FProtocolResponse FCoreService::FindAssetReferencers(const FProtocolRequest& Request) const
    {
        FProtocolResponse Strict;
        if (!RejectUnknownParams(Request, {TEXT("assetPath"), TEXT("recursive"), TEXT("cursor"), TEXT("limit")}, Strict)) return Strict;
        FString AssetPath, Cursor; bool bRecursive = false; double LimitNumber = 500;
        if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("assetPath"), AssetPath)
            || AssetPath.TrimStartAndEnd().IsEmpty())
            return ErrorResponse(Request, FProtocolError::Make(EErrorCode::InvalidArgument,
                TEXT("assetPath is required."), TEXT("FCoreService::FindAssetReferencers")));
        if (Request.Params->HasField(TEXT("recursive")) && !Request.Params->TryGetBoolField(TEXT("recursive"), bRecursive))
            return ErrorResponse(Request, FProtocolError::Make(EErrorCode::TypeMismatch,
                TEXT("recursive must be boolean."), TEXT("FCoreService::FindAssetReferencers")));
        if ((Request.Params->HasField(TEXT("cursor")) && !Request.Params->TryGetStringField(TEXT("cursor"), Cursor))
            || (Request.Params->HasField(TEXT("limit")) && !Request.Params->TryGetNumberField(TEXT("limit"), LimitNumber)))
            return ErrorResponse(Request, FProtocolError::Make(EErrorCode::TypeMismatch,
                TEXT("cursor must be a string and limit must be a number."), TEXT("FCoreService::FindAssetReferencers")));
        int32 Offset = 0;
        if (!ParseOffsetCursor(Cursor, Offset) || !FMath::IsNearlyEqual(LimitNumber, FMath::RoundToDouble(LimitNumber))
            || LimitNumber < 1 || LimitNumber > 500)
            return ErrorResponse(Request, FProtocolError::Make(EErrorCode::InvalidArgument,
                TEXT("cursor must be a non-negative decimal offset and limit must be an integer from 1 to 500."),
                TEXT("FCoreService::FindAssetReferencers")));
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>(); FProtocolError Error;
        if (!FUnrealAssetInspection::FindReferencers(AssetPath, bRecursive, Offset,
            static_cast<int32>(LimitNumber), Result, Error)) return ErrorResponse(Request, Error);
        FProtocolResponse Response; Response.Id = Request.Id; Response.IdJsonValue = Request.IdJsonValue;
        Response.Result = Result; return Response;
    }

    FProtocolResponse FCoreService::Capabilities(const FProtocolRequest& Request) const
    {
        FProtocolResponse Strict; if (!RejectUnknownParams(Request, {TEXT("domain"),TEXT("operationNames")}, Strict)) return Strict;
        FString Domain; Request.Params->TryGetStringField(TEXT("domain"), Domain); TArray<FString> Names; const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (Request.Params->TryGetArrayField(TEXT("operationNames"), Values) && Values) for (const TSharedPtr<FJsonValue>& Value : *Values) { FString Name; if (!Value.IsValid() || !Value->TryGetString(Name) || Name.IsEmpty()) return ErrorResponse(Request, FProtocolError::Make(EErrorCode::TypeMismatch, TEXT("operationNames must contain non-empty strings."), TEXT("FCoreService::Capabilities"))); Names.Add(Name); }
        FProtocolError Error; TSharedRef<FJsonObject> Result = FOperationRegistry::Get().GetCapabilities(Domain, Names, Error); if (Error.Code != EErrorCode::None) return ErrorResponse(Request, Error);
        FProtocolResponse Response; Response.Id = Request.Id; Response.IdJsonValue = Request.IdJsonValue; Response.Result = Result; return Response;
    }

    FProtocolResponse FCoreService::Inspect(const FProtocolRequest& Request) const
    {
        FProtocolResponse Strict; if (!RejectUnknownParams(Request, {TEXT("assetPath"),TEXT("facets"),TEXT("classDefaultPropertyPaths"),TEXT("componentQuery"),TEXT("cursor"),TEXT("limit")}, Strict)) return Strict;
        FString AssetPath, Cursor; double LimitNumber = 500; if (!Request.Params->TryGetStringField(TEXT("assetPath"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty()) return ErrorResponse(Request, FProtocolError::Make(EErrorCode::InvalidArgument, TEXT("assetPath is required."), TEXT("FCoreService::Inspect"))); Request.Params->TryGetStringField(TEXT("cursor"), Cursor); Request.Params->TryGetNumberField(TEXT("limit"), LimitNumber);
        int32 Offset = 0;
        if (!ParseOffsetCursor(Cursor, Offset) || !FMath::IsNearlyEqual(LimitNumber, FMath::RoundToDouble(LimitNumber)) || LimitNumber < 1 || LimitNumber > 500)
            return ErrorResponse(Request, FProtocolError::Make(EErrorCode::InvalidArgument, TEXT("cursor must be a non-negative decimal offset and limit must be an integer from 1 to 500."), TEXT("FCoreService::Inspect")));
        TArray<FString> Facets; const TArray<TSharedPtr<FJsonValue>>* Values = nullptr; if (Request.Params->TryGetArrayField(TEXT("facets"), Values) && Values) { if (Values->Num() > 32) return ErrorResponse(Request, FProtocolError::Make(EErrorCode::InvalidArgument, TEXT("facets accepts at most 32 entries."), TEXT("FCoreService::Inspect"))); for (const TSharedPtr<FJsonValue>& Value : *Values) { FString Facet; if (!Value.IsValid() || !Value->TryGetString(Facet) || Facet.IsEmpty()) return ErrorResponse(Request, FProtocolError::Make(EErrorCode::TypeMismatch, TEXT("facets must contain non-empty strings."), TEXT("FCoreService::Inspect"))); Facets.AddUnique(Facet); } }
        TArray<FString> ClassDefaultPropertyPaths;
        Values = nullptr;
        if (Request.Params->HasField(TEXT("classDefaultPropertyPaths")))
        {
            if (!Request.Params->TryGetArrayField(TEXT("classDefaultPropertyPaths"), Values) || !Values || Values->Num() > 500)
                return ErrorResponse(Request, FProtocolError::Make(EErrorCode::TypeMismatch, TEXT("classDefaultPropertyPaths must be an array of at most 500 non-empty strings."), TEXT("FCoreService::Inspect")));
            for (const TSharedPtr<FJsonValue>& Value : *Values)
            {
                FString PropertyPath;
                if (!Value.IsValid() || !Value->TryGetString(PropertyPath) || PropertyPath.TrimStartAndEnd().IsEmpty())
                    return ErrorResponse(Request, FProtocolError::Make(EErrorCode::TypeMismatch, TEXT("classDefaultPropertyPaths must contain non-empty strings."), TEXT("FCoreService::Inspect")));
                ClassDefaultPropertyPaths.AddUnique(PropertyPath);
            }
        }
        const TSharedPtr<FJsonObject>* ComponentQueryValue = nullptr;
        if (Request.Params->HasField(TEXT("componentQuery")) && !Request.Params->TryGetObjectField(TEXT("componentQuery"), ComponentQueryValue))
            return ErrorResponse(Request, FProtocolError::Make(EErrorCode::TypeMismatch, TEXT("componentQuery must be an object."), TEXT("FCoreService::Inspect")));
        const TSharedPtr<FJsonObject> ComponentQuery = ComponentQueryValue ? *ComponentQueryValue : TSharedPtr<FJsonObject>();
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>(); FProtocolError Error; if (!FBlueprintInspection::Inspect(AssetPath, Facets, ClassDefaultPropertyPaths, ComponentQuery, Offset, static_cast<int32>(LimitNumber), Result, Error)) return ErrorResponse(Request, Error);
        FProtocolResponse Response; Response.Id = Request.Id; Response.IdJsonValue = Request.IdJsonValue; Response.Result = Result; return Response;
    }

    FProtocolResponse FCoreService::Validate(const FProtocolRequest& Request) const
    {
        FProtocolResponse Strict;
        if (!RejectUnknownParams(Request, {TEXT("requestId"),TEXT("operations"),TEXT("expectedStructureHashes")}, Strict)) return Strict;
        FString RequestId;
        if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("requestId"), RequestId)
            || RequestId.TrimStartAndEnd().IsEmpty()) return RequestIdRequired(Request);
        TArray<TSharedRef<FJsonObject>> Operations;
        if (!ReadOperations(Request, Operations, Strict)) return Strict;
        TMap<FString,FString> Hashes;
        if (!ReadExpectedHashes(Request, Hashes, Strict)) return Strict;
        FJobSnapshot Snapshot;
        FProtocolError Error;
        bool bReplay = false;
        const bool bStarted = FJobManager::Get().StartRead(TEXT("blueprint.validate"), RequestId, Request.Params,
            [Operations, Hashes](FJobExecutionContext& Context, TSharedPtr<FJsonObject>& JobResult, FProtocolError& JobError)
            {
                Context.EnterPhase(EJobPhase::Preflight, true, TEXT("Validating operations and affected packages."));
                bool bSucceeded = false;
                FProtocolResponse Internal = RunOnGameThread([&Operations, &Hashes, &Context]()
                {
                    FProtocolRequest EmptyRequest;
                    FProtocolResponse Response;
                    FPreflightRequest Preflight;
                    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
                    FProtocolError Error;
                    if (!FOperationRegistry::Get().Validate(Operations, Preflight, Result, Error))
                    {
                        Response.Error = Error;
                        return Response;
                    }
                    Preflight.ExpectedStructureHashes = Hashes;
                    Context.ReportProgress(Operations.Num(), Operations.Num(), TEXT("Validated operation schemas."));
                    const FPreflightResult Check = FWritePreflight::Run(Preflight);
                    Result->SetBoolField(TEXT("valid"), Check.bSucceeded);
                    Result->SetNumberField(TEXT("requiredBytes"), static_cast<double>(Check.RequiredBytes));
                    Result->SetNumberField(TEXT("freeBytes"), static_cast<double>(Check.FreeBytes));
                    TArray<TSharedPtr<FJsonValue>> ImpactPackages;
                    for (int32 Index = 0; Index < Check.ImpactPackages.Num(); ++Index)
                    {
                        const FImpactPackage& Impact = Check.ImpactPackages[Index];
                        TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
                        Item->SetStringField(TEXT("packageName"), Impact.PackageName);
                        Item->SetStringField(TEXT("filename"), Impact.Filename);
                        Item->SetBoolField(TEXT("existsOnDisk"), Impact.bExistsOnDisk);
                        Item->SetBoolField(TEXT("wasDirty"), Impact.bWasDirty);
                        Item->SetBoolField(TEXT("readOnly"), Impact.bReadOnly);
                        TArray<TSharedPtr<FJsonValue>> Roles;
                        if (Impact.bDirectWrite) Roles.Add(MakeShared<FJsonValueString>(TEXT("directWrite")));
                        if (Impact.bCompileCheck) Roles.Add(MakeShared<FJsonValueString>(TEXT("compileCheck")));
                        if (Impact.bReferenceCheck) Roles.Add(MakeShared<FJsonValueString>(TEXT("referenceCheck")));
                        Item->SetArrayField(TEXT("roles"), Roles);
                        TArray<TSharedPtr<FJsonValue>> Reasons;
                        for (const int32 OperationIndex : Impact.OperationIndices)
                        {
                            TSharedRef<FJsonObject> Reason = MakeShared<FJsonObject>();
                            Reason->SetNumberField(TEXT("operationIndex"), OperationIndex);
                            Reason->SetStringField(TEXT("basis"), Impact.bDirectWrite ? TEXT("direct-target")
                                : Impact.bCompileCheck ? TEXT("blueprint-dependency") : TEXT("package-referencer"));
                            TArray<TSharedPtr<FJsonValue>> Sources;
                            for (const FString& Source : Impact.ReferencedFrom) Sources.Add(MakeShared<FJsonValueString>(Source));
                            Reason->SetArrayField(TEXT("referencedFrom"), Sources);
                            Reasons.Add(MakeShared<FJsonValueObject>(Reason));
                        }
                        Item->SetArrayField(TEXT("reasons"), Reasons);
                        Item->SetStringField(TEXT("beforeFileHash"), Impact.BeforeHash);
                        Item->SetStringField(TEXT("expectedStructureHash"), Impact.ExpectedHash);
                        Item->SetStringField(TEXT("actualStructureHash"), Impact.ActualStructureHash);
                        if (Impact.bHasStructureExpectation) Item->SetBoolField(TEXT("structureHashMatched"), Impact.bStructureHashMatched);
                        ImpactPackages.Add(MakeShared<FJsonValueObject>(Item));
                        Context.ReportProgress(Index + 1, Check.ImpactPackages.Num(), TEXT("Validated affected package."), Impact.PackageName);
                    }
                    Result->SetArrayField(TEXT("impactPackages"), ImpactPackages);
                    TSharedRef<FJsonObject> SourceControl = MakeShared<FJsonObject>();
                    TSharedRef<FJsonObject> EditorProvider = MakeShared<FJsonObject>();
                    EditorProvider->SetBoolField(TEXT("enabled"), Check.SourceControl.bProviderEnabled);
                    EditorProvider->SetBoolField(TEXT("available"), Check.SourceControl.bProviderAvailable);
                    EditorProvider->SetStringField(TEXT("name"), Check.SourceControl.ProviderName);
                    SourceControl->SetObjectField(TEXT("editorProvider"), EditorProvider);
                    TSharedRef<FJsonObject> WorkingCopy = MakeShared<FJsonObject>();
                    WorkingCopy->SetStringField(TEXT("kind"), Check.SourceControl.WorkingCopyKind);
                    WorkingCopy->SetStringField(TEXT("root"), Check.SourceControl.WorkingCopyRoot);
                    SourceControl->SetObjectField(TEXT("workingCopy"), WorkingCopy);
                    TArray<TSharedPtr<FJsonValue>> SourceFiles;
                    for (const FSourceControlFileState& File : Check.SourceControl.Files)
                    {
                        TSharedRef<FJsonObject> FileJson = MakeShared<FJsonObject>();
                        FileJson->SetStringField(TEXT("filename"), File.Filename);
                        FileJson->SetStringField(TEXT("editorProviderState"), !Check.SourceControl.bProviderEnabled ? TEXT("unknown")
                            : File.bAdded ? TEXT("added") : File.bCheckedOut ? TEXT("checkedOut")
                            : File.bSourceControlled ? TEXT("controlled") : TEXT("uncontrolled"));
                        FileJson->SetBoolField(TEXT("existingFile"), File.bExists);
                        FileJson->SetBoolField(TEXT("newFileNeedsAdd"), !File.bExists);
                        SourceFiles.Add(MakeShared<FJsonValueObject>(FileJson));
                    }
                    SourceControl->SetArrayField(TEXT("files"), SourceFiles);
                    Result->SetObjectField(TEXT("sourceControl"), SourceControl);
                    TArray<TSharedPtr<FJsonValue>> CompileOrder;
                    for (const FString& PackageName : Check.CompileOrder) CompileOrder.Add(MakeShared<FJsonValueString>(PackageName));
                    Result->SetArrayField(TEXT("compileOrder"), CompileOrder);
                    TArray<TSharedPtr<FJsonValue>> Issues;
                    for (const FPreflightIssue& Issue : Check.Issues)
                    {
                        TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
                        Item->SetStringField(TEXT("code"), Issue.Code); Item->SetStringField(TEXT("message"), Issue.Message);
                        Item->SetStringField(TEXT("packageName"), Issue.PackageName); Item->SetStringField(TEXT("referencePath"), Issue.ReferencePath);
                        Item->SetNumberField(TEXT("operationIndex"), Issue.OperationIndex); Issues.Add(MakeShared<FJsonValueObject>(Item));
                    }
                    Result->SetArrayField(TEXT("issues"), Issues);
                    Response.Result = Result;
                    return Response;
                });
                if (Internal.Error.IsSet()) { JobError = Internal.Error.GetValue(); return false; }
                JobResult = Internal.Result;
                bSucceeded = JobResult.IsValid() && JobResult->GetBoolField(TEXT("valid"));
                if (!bSucceeded) JobError = FProtocolError::Make(EErrorCode::ValidationFailed,
                    TEXT("Blueprint preflight validation failed."), TEXT("FWritePreflight::Run"));
                return bSucceeded;
            }, Snapshot, Error, bReplay);
        if (!bStarted) return ErrorResponse(Request, Error);
        FProtocolResponse Response; Response.Id = Request.Id; Response.IdJsonValue = Request.IdJsonValue;
        Response.Result = Snapshot.ToJson(); Response.Result->SetBoolField(TEXT("replay"), bReplay); return Response;
    }

    FProtocolResponse FCoreService::Apply(const FProtocolRequest& Request) const
    {
        FProtocolResponse Strict; if (!RejectUnknownParams(Request, {TEXT("requestId"),TEXT("operations"),TEXT("expectedStructureHashes")}, Strict)) return Strict;
        FString RequestId; Request.Params->TryGetStringField(TEXT("requestId"), RequestId); TArray<TSharedRef<FJsonObject>> Operations; if (!ReadOperations(Request, Operations, Strict)) return Strict; TMap<FString,FString> Hashes; if (!ReadExpectedHashes(Request, Hashes, Strict)) return Strict;
        FWritePipelineRequest PipelineRequest; FProtocolError Error; if (!FOperationRegistry::Get().BuildWriteRequest(RequestId, Operations, Hashes, PipelineRequest, Error)) return ErrorResponse(Request, Error);
        FJobSnapshot Snapshot; bool bReplay = false;
        const bool bStarted = FJobManager::Get().StartWrite(TEXT("blueprint.apply"), RequestId, Request.Params,
            [PipelineRequest](FJobExecutionContext& Context, TSharedPtr<FJsonObject>& Result, FProtocolError& JobError)
            {
                FWritePipelineProgress Progress;
                Progress.EnterPhase = [&Context](const FString& Phase, const bool bSafe, const FString& Message) { return Context.EnterPhase(PhaseFromString(Phase), bSafe, Message); };
                Progress.Report = [&Context](const int32 Completed, const int32 Total, const FString& Message, const FString& AssetPath) { Context.ReportProgress(Completed, Total, Message, AssetPath); };
                Progress.IsCancellationRequested = [&Context]() { return Context.IsCancellationRequested(); }; Progress.Heartbeat = [&Context]() { Context.Heartbeat(); };
                const FWritePipelineResult PipelineResult = FWritePipeline::Execute(PipelineRequest, Progress); Result = PipelineResult.ToJson();
                if (!PipelineResult.bSucceeded) { JobError = FProtocolError::Make(EErrorCode::ValidationFailed, PipelineResult.Error.Message, PipelineResult.Error.UECallsite); JobError.AssetPath = PipelineResult.Error.AssetPath; JobError.OperationIndex = PipelineResult.Error.OperationIndex; JobError.CompilerMessages = PipelineResult.Error.CompilerMessages; }
                return PipelineResult.bSucceeded;
            }, Snapshot, Error, bReplay);
        if (!bStarted) return ErrorResponse(Request, Error);
        FProtocolResponse Response; Response.Id = Request.Id; Response.IdJsonValue = Request.IdJsonValue; Response.Result = Snapshot.ToJson(); Response.Result->SetBoolField(TEXT("replay"), bReplay); return Response;
    }

    FProtocolResponse FCoreService::Verify(const FProtocolRequest& Request) const
    {
        FProtocolResponse Strict; if (!RejectUnknownParams(Request, {TEXT("requestId"),TEXT("assetPaths"),TEXT("expectations"),TEXT("compile"),TEXT("reload")}, Strict)) return Strict;
        FString RequestId; if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("requestId"), RequestId) || RequestId.TrimStartAndEnd().IsEmpty()) return RequestIdRequired(Request);
        const TArray<TSharedPtr<FJsonValue>>* Paths = nullptr; if (!Request.Params->TryGetArrayField(TEXT("assetPaths"), Paths) || !Paths || !Paths->Num()) return ErrorResponse(Request, FProtocolError::Make(EErrorCode::InvalidArgument, TEXT("assetPaths must be a non-empty array."), TEXT("FCoreService::Verify")));
        TArray<FString> AssetPaths; for (const TSharedPtr<FJsonValue>& Value : *Paths) { FString Path; if (!Value.IsValid() || !Value->TryGetString(Path) || Path.IsEmpty()) return ErrorResponse(Request, FProtocolError::Make(EErrorCode::TypeMismatch, TEXT("assetPaths must contain non-empty strings."), TEXT("FCoreService::Verify"))); AssetPaths.Add(Path); }
        const TArray<TSharedPtr<FJsonValue>>* Expectations = nullptr; static const TArray<TSharedPtr<FJsonValue>> Empty; if (Request.Params->HasField(TEXT("expectations")) && !Request.Params->TryGetArrayField(TEXT("expectations"), Expectations)) return ErrorResponse(Request, FProtocolError::Make(EErrorCode::TypeMismatch, TEXT("expectations must be an array."), TEXT("FCoreService::Verify")));
        bool bCompile = true, bReload = true; Request.Params->TryGetBoolField(TEXT("compile"), bCompile); Request.Params->TryGetBoolField(TEXT("reload"), bReload);
        const TArray<TSharedPtr<FJsonValue>> ExpectationValues = Expectations ? *Expectations : Empty;
        FJobSnapshot Snapshot; FProtocolError Error; bool bReplay = false;
        const bool bStarted = FJobManager::Get().StartRead(TEXT("blueprint.verify"), RequestId, Request.Params,
            [AssetPaths, ExpectationValues, bCompile, bReload](FJobExecutionContext& Context, TSharedPtr<FJsonObject>& JobResult, FProtocolError& JobError)
            {
                if (bReload) Context.EnterPhase(EJobPhase::Reload, false, TEXT("Reloading verification packages from disk."));
                else if (bCompile) Context.EnterPhase(EJobPhase::Compile, false, TEXT("Compiling verification Blueprints."));
                else Context.EnterPhase(EJobPhase::Verify, true, TEXT("Verifying Blueprint structure."));
                FProtocolResponse Internal = RunOnGameThread([&AssetPaths, &ExpectationValues, bCompile, bReload, &Context]()
                {
                    FProtocolResponse Response; TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>(); FProtocolError Error;
                    if (!FBlueprintVerification::Verify(AssetPaths, ExpectationValues, bCompile, bReload, Result, Error,
                        [&Context](const FString& Stage, const int32 Completed, const int32 Total, const FString& AssetPath)
                        {
                            Context.ReportProgress(Completed, Total, Stage + TEXT(" Blueprint."), AssetPath);
                        })) { Response.Error = Error; return Response; }
                    Response.Result = Result; return Response;
                });
                if (Internal.Error.IsSet()) { JobError = Internal.Error.GetValue(); return false; }
                Context.EnterPhase(EJobPhase::Verify, true, TEXT("Blueprint verification completed."));
                JobResult = Internal.Result; return true;
            }, Snapshot, Error, bReplay);
        if (!bStarted) return ErrorResponse(Request, Error);
        FProtocolResponse Response; Response.Id = Request.Id; Response.IdJsonValue = Request.IdJsonValue;
        Response.Result = Snapshot.ToJson(); Response.Result->SetBoolField(TEXT("replay"), bReplay); return Response;
    }

    FProtocolResponse FCoreService::GetJob(const FProtocolRequest& Request) const
    {
        FProtocolResponse Strict; if (!RejectUnknownParams(Request, {TEXT("action"),TEXT("jobId"),TEXT("requestId")}, Strict)) return Strict;
        FJobSnapshot Snapshot;
        if (!ResolveJob(Request, Snapshot))
            return InvalidJobRequest(Request, TEXT("Unknown jobId or requestId."), TEXT("FCoreService::GetJob"));
        FProtocolResponse Response;
        Response.Id = Request.Id;
        Response.IdJsonValue = Request.IdJsonValue;
        Response.Result = Snapshot.ToJson();
        return Response;
    }

    FProtocolResponse FCoreService::CancelJob(const FProtocolRequest& Request) const
    {
        FProtocolResponse Strict; if (!RejectUnknownParams(Request, {TEXT("action"),TEXT("jobId")}, Strict)) return Strict;
        if (!Request.Params.IsValid())
            return InvalidJobRequest(Request, TEXT("blueprint.job cancel requires jobId."), TEXT("FCoreService::CancelJob"));
        FString JobId;
        if (!Request.Params->TryGetStringField(TEXT("jobId"), JobId) || JobId.IsEmpty())
            return InvalidJobRequest(Request, TEXT("blueprint.job cancel requires jobId."), TEXT("FCoreService::CancelJob"));
        FJobSnapshot Snapshot;
        FProtocolError Error;
        if (!FJobManager::Get().Cancel(JobId, Snapshot, Error))
        {
            FProtocolResponse Response;
            Response.Id = Request.Id;
            Response.IdJsonValue = Request.IdJsonValue;
            Response.Error = Error;
            return Response;
        }
        FProtocolResponse Response;
        Response.Id = Request.Id;
        Response.IdJsonValue = Request.IdJsonValue;
        Response.Result = Snapshot.ToJson();
        return Response;
    }

    FProtocolResponse FCoreService::GetRequestJournal(const FProtocolRequest& Request) const
    {
        FString Action;
        if (!Request.Params.IsValid() || !Request.Params->TryGetStringField(TEXT("action"), Action))
            return InvalidJobRequest(Request, TEXT("blueprint.request requires action query or status."), TEXT("FCoreService::GetRequestJournal"));
        FProtocolResponse Response;
        Response.Id = Request.Id;
        Response.IdJsonValue = Request.IdJsonValue;
        FProtocolError Error;
        if (Action == TEXT("status"))
        {
            FProtocolResponse Strict;
            if (!RejectUnknownParams(Request, {TEXT("action")}, Strict)) return Strict;
            FRequestJournalStatus Status;
            if (!FRequestJournal::Get().GetStatus(Status, Error))
            {
                Response.Error = Error;
                return Response;
            }
            Response.Result = Status.ToJson();
            return Response;
        }
        if (Action == TEXT("query"))
        {
            FProtocolResponse Strict;
            if (!RejectUnknownParams(Request, {TEXT("action"), TEXT("requestId")}, Strict)) return Strict;
            FString RequestId;
            if (!Request.Params->TryGetStringField(TEXT("requestId"), RequestId) || RequestId.TrimStartAndEnd().IsEmpty())
                return InvalidJobRequest(Request, TEXT("blueprint.request query requires requestId."), TEXT("FCoreService::GetRequestJournal"));
            FRequestJournalRecord Record;
            if (!FRequestJournal::Get().Query(RequestId, Record, Error))
            {
                Response.Error = Error;
                return Response;
            }
            Response.Result = Record.ToStatusJson();
            return Response;
        }
        return InvalidJobRequest(Request, TEXT("Unknown blueprint.request action."), TEXT("FCoreService::GetRequestJournal"));
    }
}
