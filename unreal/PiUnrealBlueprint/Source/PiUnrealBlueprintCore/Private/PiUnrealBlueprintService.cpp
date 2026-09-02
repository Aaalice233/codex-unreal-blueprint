#include "PiUnrealBlueprintService.h"

namespace PiUnrealBlueprint
{
    FCoreService& FCoreService::Get()
    {
        static FCoreService Instance;
        return Instance;
    }

    FProtocolResponse FCoreService::Dispatch(const FProtocolRequest& Request) const
    {
        if (Request.Method == TEXT("unreal_status"))
        {
            return GetStatus(Request);
        }
        return MakeNotImplemented(Request.Id, Request.Method);
    }

    FProtocolResponse FCoreService::MakeNotImplemented(const FString& RequestId, const FString& Method) const
    {
        FProtocolResponse Response;
        Response.Id = RequestId;
        Response.Error = FProtocolError::Make(EErrorCode::NotImplemented,
            FString::Printf(TEXT("Method '%s' is not implemented in plugin skeleton %s."), *Method, PluginVersion),
            TEXT("FCoreService::Dispatch"));
        return Response;
    }

    FProtocolResponse FCoreService::GetStatus(const FProtocolRequest& Request) const
    {
        FProtocolResponse Response;
        Response.Id = Request.Id;
        Response.Result = MakeShared<FJsonObject>();
        Response.Result->SetStringField(TEXT("pluginVersion"), PluginVersion);
        Response.Result->SetStringField(TEXT("protocolVersion"), ProtocolVersion);
        Response.Result->SetStringField(TEXT("serviceState"), LexToString(EServiceState::Unavailable));
        Response.Result->SetBoolField(TEXT("coreAvailable"), true);
        Response.Result->SetBoolField(TEXT("transportAvailable"), false);
        Response.Result->SetStringField(TEXT("unavailableReason"), TEXT("NotImplemented: TCP transport and Blueprint operations are not implemented."));
        TArray<TSharedPtr<FJsonValue>> Methods;
        Methods.Add(MakeShared<FJsonValueString>(TEXT("unreal_status")));
        Response.Result->SetArrayField(TEXT("implementedMethods"), Methods);
        return Response;
    }
}
