#include "Misc/AutomationTest.h"
#include "PiUnrealBlueprintProtocol.h"
#include "PiUnrealBlueprintService.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPiProtocolParsesStatusRequest,
    "PiUnrealBlueprint.Protocol.ParsesStatusRequest",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPiProtocolParsesStatusRequest::RunTest(const FString& Parameters)
{
    const FString Json = TEXT("{\"jsonrpc\":\"2.0\",\"id\":\"request-1\",\"method\":\"unreal_status\",\"params\":{}}");
    PiUnrealBlueprint::FProtocolRequest Request;
    PiUnrealBlueprint::FProtocolError Error;
    TestTrue(TEXT("Valid request parses"), PiUnrealBlueprint::FProtocolRequest::Parse(Json, Request, Error));
    TestEqual(TEXT("Method is preserved"), Request.Method, FString(TEXT("unreal_status")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPiProtocolRejectsUnknownField,
    "PiUnrealBlueprint.Protocol.RejectsUnknownField",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPiProtocolRejectsUnknownField::RunTest(const FString& Parameters)
{
    const FString Json = TEXT("{\"jsonrpc\":\"2.0\",\"id\":\"request-1\",\"method\":\"unreal_status\",\"extra\":true}");
    PiUnrealBlueprint::FProtocolRequest Request;
    PiUnrealBlueprint::FProtocolError Error;
    TestFalse(TEXT("Unknown field is rejected"), PiUnrealBlueprint::FProtocolRequest::Parse(Json, Request, Error));
    TestTrue(TEXT("Stable InvalidRequest code is returned"), Error.Code == PiUnrealBlueprint::EErrorCode::InvalidRequest);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPiUnsupportedMethodIsExplicit,
    "PiUnrealBlueprint.Core.UnsupportedMethodIsNotImplemented",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPiUnsupportedMethodIsExplicit::RunTest(const FString& Parameters)
{
    PiUnrealBlueprint::FProtocolRequest Request;
    Request.JsonRpc = TEXT("2.0");
    Request.Id = TEXT("request-2");
    Request.Method = TEXT("blueprint_apply");
    Request.Params = MakeShared<FJsonObject>();
    const PiUnrealBlueprint::FProtocolResponse Response = PiUnrealBlueprint::FCoreService::Get().Dispatch(Request);
    TestFalse(TEXT("Unsupported method cannot report success"), Response.IsSuccess());
    TestTrue(TEXT("Stable NotImplemented code is returned"),
        Response.Error.IsSet() && Response.Error.GetValue().Code == PiUnrealBlueprint::EErrorCode::NotImplemented);
    return true;
}

#endif
