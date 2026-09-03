#include "PiUnrealBlueprintService.h"

#include "PiUnrealBlueprintJobs.h"
#include "PiUnrealBlueprintInspection.h"
#include "PiUnrealBlueprintOperationRegistry.h"
#include "PiUnrealBlueprintPreflight.h"
#include "PiUnrealBlueprintRequestJournal.h"
#include "PiUnrealBlueprintRuntimeStatus.h"
#include "PiUnrealBlueprintSearch.h"
#include "PiUnrealBlueprintSourceControl.h"
#include "PiUnrealBlueprintVerification.h"
#include "PiUnrealBlueprintWritePipeline.h"

#include "Async/Async.h"
#include "Editor.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "UObject/UObjectIterator.h"

namespace PiUnrealBlueprint
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
                TEXT("Every write request requires a non-empty requestId in params."), TEXT("FCoreService::Dispatch"));
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
            AsyncTask(ENamedThreads::GameThread, [&Response, &Work, Event]() { Response = Work(); Event->Trigger(); });
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
        if (Request.Method == TEXT("blueprint.capabilities")) return Capabilities(Request);
        if (Request.Method == TEXT("blueprint.inspect")) return RunOnGameThread([this, &Request]() { return Inspect(Request); });
        if (Request.Method == TEXT("blueprint.validate")) return RunOnGameThread([this, &Request]() { return Validate(Request); });
        if (Request.Method == TEXT("blueprint.apply")) return Apply(Request);
        if (Request.Method == TEXT("blueprint.verify")) return RunOnGameThread([this, &Request]() { return Verify(Request); });
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
        const TCHAR* Implemented[] = {TEXT("unreal.status"),TEXT("unreal.doctor"),TEXT("unreal.search"),TEXT("blueprint.capabilities"),TEXT("blueprint.inspect"),TEXT("blueprint.validate"),TEXT("blueprint.apply"),TEXT("blueprint.job"),TEXT("blueprint.verify")};
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
        SourceControlJson->SetBoolField(TEXT("providerEnabled"), SourceControl.bProviderEnabled);
        SourceControlJson->SetBoolField(TEXT("providerAvailable"), SourceControl.bProviderAvailable);
        SourceControlJson->SetStringField(TEXT("providerName"), SourceControl.ProviderName);
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
        FProtocolResponse Strict; if (!RejectUnknownParams(Request, {TEXT("assetPath"),TEXT("facets"),TEXT("classDefaultPropertyPaths"),TEXT("cursor"),TEXT("limit")}, Strict)) return Strict;
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
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>(); FProtocolError Error; if (!FBlueprintInspection::Inspect(AssetPath, Facets, ClassDefaultPropertyPaths, Offset, static_cast<int32>(LimitNumber), Result, Error)) return ErrorResponse(Request, Error);
        FProtocolResponse Response; Response.Id = Request.Id; Response.IdJsonValue = Request.IdJsonValue; Response.Result = Result; return Response;
    }

    FProtocolResponse FCoreService::Validate(const FProtocolRequest& Request) const
    {
        FProtocolResponse Strict; if (!RejectUnknownParams(Request, {TEXT("operations"),TEXT("expectedStructureHashes")}, Strict)) return Strict;
        TArray<TSharedRef<FJsonObject>> Operations; if (!ReadOperations(Request, Operations, Strict)) return Strict; TMap<FString,FString> Hashes; if (!ReadExpectedHashes(Request, Hashes, Strict)) return Strict;
        FPreflightRequest Preflight; TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>(); FProtocolError Error; if (!FOperationRegistry::Get().Validate(Operations, Preflight, Result, Error)) return ErrorResponse(Request, Error); Preflight.ExpectedStateHashes = Hashes;
        const FPreflightResult Check = FWritePreflight::Run(Preflight);
        Result->SetBoolField(TEXT("valid"), Check.bSucceeded);
        Result->SetNumberField(TEXT("requiredBytes"), static_cast<double>(Check.RequiredBytes));
        Result->SetNumberField(TEXT("freeBytes"), static_cast<double>(Check.FreeBytes));
        TArray<TSharedPtr<FJsonValue>> ImpactPackages;
        for (const FImpactPackage& Impact : Check.ImpactPackages)
        {
            TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("packageName"), Impact.PackageName);
            Item->SetStringField(TEXT("filename"), Impact.Filename);
            Item->SetBoolField(TEXT("existsOnDisk"), Impact.bExistsOnDisk);
            Item->SetBoolField(TEXT("wasDirty"), Impact.bWasDirty);
            Item->SetBoolField(TEXT("readOnly"), Impact.bReadOnly);
            Item->SetStringField(TEXT("beforeHash"), Impact.BeforeHash);
            Item->SetStringField(TEXT("expectedHash"), Impact.ExpectedHash);
            ImpactPackages.Add(MakeShared<FJsonValueObject>(Item));
        }
        Result->SetArrayField(TEXT("impactPackages"), ImpactPackages);
        TArray<TSharedPtr<FJsonValue>> CompileOrder;
        for (const FString& PackageName : Check.CompileOrder)
            CompileOrder.Add(MakeShared<FJsonValueString>(PackageName));
        Result->SetArrayField(TEXT("compileOrder"), CompileOrder);
        TSharedRef<FJsonObject> SourceControl = MakeShared<FJsonObject>();
        SourceControl->SetBoolField(TEXT("succeeded"), Check.SourceControl.bSucceeded);
        SourceControl->SetBoolField(TEXT("providerEnabled"), Check.SourceControl.bProviderEnabled);
        SourceControl->SetBoolField(TEXT("providerAvailable"), Check.SourceControl.bProviderAvailable);
        SourceControl->SetStringField(TEXT("providerName"), Check.SourceControl.ProviderName);
        SourceControl->SetStringField(TEXT("error"), Check.SourceControl.Error);
        TArray<TSharedPtr<FJsonValue>> SourceControlFiles;
        for (const FSourceControlFileState& File : Check.SourceControl.Files)
        {
            TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("filename"), File.Filename);
            Item->SetBoolField(TEXT("exists"), File.bExists);
            Item->SetBoolField(TEXT("readOnly"), File.bReadOnly);
            Item->SetBoolField(TEXT("sourceControlled"), File.bSourceControlled);
            Item->SetBoolField(TEXT("checkedOut"), File.bCheckedOut);
            Item->SetBoolField(TEXT("canCheckout"), File.bCanCheckout);
            Item->SetStringField(TEXT("checkedOutBy"), File.CheckedOutBy);
            SourceControlFiles.Add(MakeShared<FJsonValueObject>(Item));
        }
        SourceControl->SetArrayField(TEXT("files"), SourceControlFiles);
        Result->SetObjectField(TEXT("sourceControl"), SourceControl);
        TArray<TSharedPtr<FJsonValue>> Issues;
        for (const FPreflightIssue& Issue : Check.Issues)
        {
            TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("code"), Issue.Code);
            Item->SetStringField(TEXT("message"), Issue.Message);
            Item->SetStringField(TEXT("packageName"), Issue.PackageName);
            Item->SetStringField(TEXT("referencePath"), Issue.ReferencePath);
            Item->SetNumberField(TEXT("operationIndex"), Issue.OperationIndex);
            Issues.Add(MakeShared<FJsonValueObject>(Item));
        }
        Result->SetArrayField(TEXT("issues"), Issues);
        FProtocolResponse Response; Response.Id = Request.Id; Response.IdJsonValue = Request.IdJsonValue; Response.Result = Result; return Response;
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
        FProtocolResponse Strict; if (!RejectUnknownParams(Request, {TEXT("assetPaths"),TEXT("expectations"),TEXT("compile"),TEXT("reload")}, Strict)) return Strict;
        const TArray<TSharedPtr<FJsonValue>>* Paths = nullptr; if (!Request.Params->TryGetArrayField(TEXT("assetPaths"), Paths) || !Paths || !Paths->Num()) return ErrorResponse(Request, FProtocolError::Make(EErrorCode::InvalidArgument, TEXT("assetPaths must be a non-empty array."), TEXT("FCoreService::Verify")));
        TArray<FString> AssetPaths; for (const TSharedPtr<FJsonValue>& Value : *Paths) { FString Path; if (!Value.IsValid() || !Value->TryGetString(Path) || Path.IsEmpty()) return ErrorResponse(Request, FProtocolError::Make(EErrorCode::TypeMismatch, TEXT("assetPaths must contain non-empty strings."), TEXT("FCoreService::Verify"))); AssetPaths.Add(Path); }
        const TArray<TSharedPtr<FJsonValue>>* Expectations = nullptr; static const TArray<TSharedPtr<FJsonValue>> Empty; if (Request.Params->HasField(TEXT("expectations")) && !Request.Params->TryGetArrayField(TEXT("expectations"), Expectations)) return ErrorResponse(Request, FProtocolError::Make(EErrorCode::TypeMismatch, TEXT("expectations must be an array."), TEXT("FCoreService::Verify")));
        bool bCompile = true, bReload = true; Request.Params->TryGetBoolField(TEXT("compile"), bCompile); Request.Params->TryGetBoolField(TEXT("reload"), bReload); TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>(); FProtocolError Error; if (!FBlueprintVerification::Verify(AssetPaths, Expectations ? *Expectations : Empty, bCompile, bReload, Result, Error)) return ErrorResponse(Request, Error);
        FProtocolResponse Response; Response.Id = Request.Id; Response.IdJsonValue = Request.IdJsonValue; Response.Result = Result; return Response;
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
