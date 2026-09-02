#include "PiUnrealBlueprintCommandlet.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "PiUnrealBlueprintProtocol.h"
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
    FString RequestPath;
    FString ResultPath;
    FParse::Value(*Params, TEXT("Request="), RequestPath);
    FParse::Value(*Params, TEXT("Result="), ResultPath);

    if (RequestPath.IsEmpty() || ResultPath.IsEmpty())
    {
        UE_LOG(LogPiUnrealBlueprintCommandlet, Error,
            TEXT("InvalidRequest: both -Request=<path> and -Result=<path> are required."));
        return 2;
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
            Response.Error = ParseError;
        }
        else
        {
            Response = PiUnrealBlueprint::FCoreService::Get().Dispatch(Request);
        }
    }

    if (!WriteResponse(ResultPath, Response.ToJsonString()))
    {
        UE_LOG(LogPiUnrealBlueprintCommandlet, Error,
            TEXT("InternalError: failed to write result file '%s' at FFileHelper::SaveStringToFile."), *ResultPath);
        return 4;
    }

    if (!Response.IsSuccess())
    {
        const PiUnrealBlueprint::FProtocolError& Error = Response.Error.GetValue();
        UE_LOG(LogPiUnrealBlueprintCommandlet, Error, TEXT("%s: %s"),
            PiUnrealBlueprint::LexToString(Error.Code), *Error.Message);
        return Error.Code == PiUnrealBlueprint::EErrorCode::NotImplemented ? 3 : 2;
    }
    return 0;
}

bool UPiUnrealBlueprintCommandlet::WriteResponse(const FString& ResultPath, const FString& Json) const
{
    const FString ResultDirectory = FPaths::GetPath(ResultPath);
    if (!ResultDirectory.IsEmpty() && !IFileManager::Get().MakeDirectory(*ResultDirectory, true))
    {
        return false;
    }
    return FFileHelper::SaveStringToFile(Json, *ResultPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
