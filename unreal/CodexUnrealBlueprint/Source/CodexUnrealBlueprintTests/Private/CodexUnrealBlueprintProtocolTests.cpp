#include "Misc/AutomationTest.h"
#include "CodexUnrealBlueprintProtocol.h"
#include "CodexUnrealBlueprintService.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodexProtocolParsesStatusRequest,
    "CodexUnrealBlueprint.Protocol.ParsesStatusRequest",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexProtocolParsesStatusRequest::RunTest(const FString& Parameters)
{
    const FString Json = TEXT("{\"jsonrpc\":\"2.0\",\"id\":\"request-1\",\"method\":\"unreal_status\",\"params\":{}}");
    CodexUnrealBlueprint::FProtocolRequest Request;
    CodexUnrealBlueprint::FProtocolError Error;
    TestTrue(TEXT("Valid request parses"), CodexUnrealBlueprint::FProtocolRequest::Parse(Json, Request, Error));
    TestEqual(TEXT("Method is preserved"), Request.Method, FString(TEXT("unreal_status")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodexProtocolRejectsUnknownField,
    "CodexUnrealBlueprint.Protocol.RejectsUnknownField",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexProtocolRejectsUnknownField::RunTest(const FString& Parameters)
{
    const FString Json = TEXT("{\"jsonrpc\":\"2.0\",\"id\":\"request-1\",\"method\":\"unreal_status\",\"extra\":true}");
    CodexUnrealBlueprint::FProtocolRequest Request;
    CodexUnrealBlueprint::FProtocolError Error;
    TestFalse(TEXT("Unknown field is rejected"), CodexUnrealBlueprint::FProtocolRequest::Parse(Json, Request, Error));
    TestTrue(TEXT("Stable InvalidRequest code is returned"), Error.Code == CodexUnrealBlueprint::EErrorCode::InvalidRequest);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodexUnsupportedMethodIsExplicit,
    "CodexUnrealBlueprint.Core.UnsupportedMethodIsNotImplemented",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexUnsupportedMethodIsExplicit::RunTest(const FString& Parameters)
{
    CodexUnrealBlueprint::FProtocolRequest Request;
    Request.JsonRpc = TEXT("2.0");
    Request.Id = TEXT("request-2");
    Request.Method = TEXT("blueprint_apply");
    Request.Params = MakeShared<FJsonObject>();
    const CodexUnrealBlueprint::FProtocolResponse Response = CodexUnrealBlueprint::FCoreService::Get().Dispatch(Request);
    TestFalse(TEXT("Unsupported method cannot report success"), Response.IsSuccess());
    TestTrue(TEXT("Stable NotImplemented code is returned"),
        Response.Error.IsSet() && Response.Error.GetValue().Code == CodexUnrealBlueprint::EErrorCode::NotImplemented);
    return true;
}

#endif
