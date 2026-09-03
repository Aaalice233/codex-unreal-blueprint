#include "PiUnrealBlueprintCommandlet.h"

#include "Async/TaskGraphInterfaces.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "PiUnrealBlueprintJobs.h"
#include "PiUnrealBlueprintProtocol.h"
#include "PiUnrealBlueprintRuntimeStatus.h"
#include "PiUnrealBlueprintService.h"

DEFINE_LOG_CATEGORY_STATIC(LogPiUnrealBlueprintCommandlet, Log, All);

UPiUnrealBlueprintCommandlet::UPiUnrealBlueprintCommandlet(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    IsClient = false;
    IsServer = false;
    IsEditor = true;
    LogToConsole = true;
    ShowErrorCount = true;
    HelpDescription = TEXT("Dispatches one JSON request through PiUnrealBlueprint Core.");
    HelpUsage = TEXT("-run=PiUnrealBlueprint -Request=<request.json> -Result=<result.json>");
}

int32 UPiUnrealBlueprintCommandlet::Main(const FString& Params)
{
    // Commandlet 只经 Core 分发，不启动 Editor TCP Transport。
    PiUnrealBlueprint::FRuntimeStatusRegistry::Get().SetTransportStatus(
        PiUnrealBlueprint::EServiceState::Unavailable, false);

    FString RequestPath;
    FString ResultPath;
    FParse::Value(*Params, TEXT("Request="), RequestPath);
    FParse::Value(*Params, TEXT("Result="), ResultPath);

    if (RequestPath.IsEmpty() || ResultPath.IsEmpty())
    {
        UE_LOG(LogPiUnrealBlueprintCommandlet, Error,
            TEXT("InvalidRequest: both -Request=<path> and -Result=<path> are required."));
        return static_cast<int32>(EExitCode::InvalidInvocationOrRequest);
    }

    RequestPath = FPaths::ConvertRelativePathToFull(RequestPath);
    ResultPath = FPaths::ConvertRelativePathToFull(ResultPath);

    FString RequestJson;
    PiUnrealBlueprint::FProtocolResponse Response;
    if (!FFileHelper::LoadFileToString(RequestJson, *RequestPath))
    {
        Response.Error = PiUnrealBlueprint::FProtocolError::Make(
            PiUnrealBlueprint::EErrorCode::InvalidRequest,
            FString::Printf(TEXT("Failed to read request file '%s'."), *RequestPath),
            TEXT("FFileHelper::LoadFileToString"));
    }
    else
    {
        PiUnrealBlueprint::FProtocolRequest Request;
        PiUnrealBlueprint::FProtocolError ParseError;
        if (!PiUnrealBlueprint::FProtocolRequest::Parse(RequestJson, Request, ParseError))
        {
            Response.Id = Request.Id;
            Response.IdJsonValue = Request.IdJsonValue;
            Response.Error = ParseError;
        }
        else
        {
            Response = PiUnrealBlueprint::FCoreService::Get().Dispatch(Request);
            Response.Id = Request.Id;
            Response.IdJsonValue = Request.IdJsonValue;
            PiUnrealBlueprint::CompleteCommandletApply(Request.Method, Response);
        }
    }

    if (!WriteResponseAtomically(ResultPath, Response.ToJsonString()))
    {
        UE_LOG(LogPiUnrealBlueprintCommandlet, Error,
            TEXT("InternalError: failed to atomically write result file '%s'."), *ResultPath);
        return static_cast<int32>(EExitCode::ResultIoFailure);
    }

    if (!Response.IsSuccess())
    {
        const PiUnrealBlueprint::FProtocolError& Error = Response.Error.GetValue();
        UE_LOG(LogPiUnrealBlueprintCommandlet, Error, TEXT("%s: %s (%s)"),
            PiUnrealBlueprint::LexToString(Error.Code), *Error.Message, *Error.UECallsite);
    }
    return static_cast<int32>(ExitCodeForResponse(Response));
}

bool UPiUnrealBlueprintCommandlet::WriteResponseAtomically(const FString& ResultPath, const FString& Json) const
{
    const FString ResultDirectory = FPaths::GetPath(ResultPath);
    if (!ResultDirectory.IsEmpty() && !IFileManager::Get().MakeDirectory(*ResultDirectory, true))
    {
        return false;
    }

    // 同目录替换保证结果文件不会暴露半写内容。
    const FString TemporaryPath = FString::Printf(TEXT("%s.%s.tmp"),
        *ResultPath, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    if (!FFileHelper::SaveStringToFile(Json, *TemporaryPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        IFileManager::Get().Delete(*TemporaryPath, false, true, true);
        return false;
    }
    if (!IFileManager::Get().Move(*ResultPath, *TemporaryPath, true, true, false, true))
    {
        IFileManager::Get().Delete(*TemporaryPath, false, true, true);
        return false;
    }
    return true;
}

void PiUnrealBlueprint::CompleteCommandletApply(
    const FString& Method, FProtocolResponse& Response)
{
    if (Method != TEXT("blueprint.apply") || !Response.IsSuccess() || !Response.Result.IsValid())
    {
        return;
    }

    FString JobId;
    bool bTerminal = false;
    bool bReplay = false;
    if (!Response.Result->TryGetStringField(TEXT("jobId"), JobId) || JobId.IsEmpty())
    {
        Response.Result.Reset();
        Response.Error = FProtocolError::Make(
            EErrorCode::InternalError,
            TEXT("Core blueprint.apply response did not contain a jobId."),
            TEXT("PiUnrealBlueprint::CompleteCommandletApply"));
        return;
    }
    Response.Result->TryGetBoolField(TEXT("terminal"), bTerminal);
    Response.Result->TryGetBoolField(TEXT("replay"), bReplay);
    if (bTerminal)
    {
        return;
    }

    FJobSnapshot Snapshot;
    for (;;)
    {
        // Commandlet 没有 Editor 帧循环，必须主动驱动 Core 队列和回到 GameThread 的 UObject 工作。
        if (FTaskGraphInterface::IsRunning())
        {
            FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
        }
        FJobManager::Get().Tick(FPlatformTime::Seconds());
        if (!FJobManager::Get().Get(JobId, Snapshot))
        {
            Response.Result.Reset();
            Response.Error = FProtocolError::Make(
                EErrorCode::InternalError,
                FString::Printf(TEXT("Core returned unknown jobId '%s'."), *JobId),
                TEXT("PiUnrealBlueprint::CompleteCommandletApply"));
            return;
        }
        if (Snapshot.bTerminal)
        {
            // 失败也返回完整 Job 快照，与 interactive job query 保持同一结果结构。
            Response.Result = Snapshot.ToJson();
            Response.Result->SetBoolField(TEXT("replay"), bReplay);
            return;
        }
        FPlatformProcess::Sleep(0.01f);
    }
}

UPiUnrealBlueprintCommandlet::EExitCode UPiUnrealBlueprintCommandlet::ExitCodeForResponse(
    const PiUnrealBlueprint::FProtocolResponse& Response)
{
    if (Response.IsSuccess())
    {
        return EExitCode::Success;
    }

    const PiUnrealBlueprint::EErrorCode ErrorCode = Response.Error.GetValue().Code;
    if (ErrorCode == PiUnrealBlueprint::EErrorCode::InvalidJson
        || ErrorCode == PiUnrealBlueprint::EErrorCode::InvalidRequest
        || ErrorCode == PiUnrealBlueprint::EErrorCode::ProtocolVersionMismatch
        || ErrorCode == PiUnrealBlueprint::EErrorCode::RequestIdRequired)
    {
        return EExitCode::InvalidInvocationOrRequest;
    }
    return EExitCode::RequestFailed;
}
