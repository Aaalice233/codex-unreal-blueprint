#include "Misc/AutomationTest.h"
#include "CodexUnrealBlueprintFraming.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodexLengthPrefixedFrameRoundTrip,
    "CodexUnrealBlueprint.Transport.LengthPrefixedFrameRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexLengthPrefixedFrameRoundTrip::RunTest(const FString& Parameters)
{
    const FString Expected = TEXT("{\"jsonrpc\":\"2.0\",\"method\":\"unreal_status\"}");
    TArray<uint8> Buffer;
    FString Error;
    TestTrue(TEXT("Frame encodes"), CodexUnrealBlueprint::FLengthPrefixedJsonFraming::Encode(Expected, Buffer, Error));

    FString Actual;
    const CodexUnrealBlueprint::EFrameDecodeResult Result =
        CodexUnrealBlueprint::FLengthPrefixedJsonFraming::TryDecode(Buffer, Actual, Error);
    TestTrue(TEXT("Complete frame decodes"), Result == CodexUnrealBlueprint::EFrameDecodeResult::Complete);
    TestEqual(TEXT("UTF-8 payload round-trips"), Actual, Expected);
    TestEqual(TEXT("Consumed frame is removed"), Buffer.Num(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodexLengthPrefixedFrameWaitsForPayload,
    "CodexUnrealBlueprint.Transport.LengthPrefixedFrameWaitsForPayload",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexLengthPrefixedFrameWaitsForPayload::RunTest(const FString& Parameters)
{
    TArray<uint8> Buffer = { 0, 0, 0, 5, static_cast<uint8>('a') };
    FString Json;
    FString Error;
    const CodexUnrealBlueprint::EFrameDecodeResult Result =
        CodexUnrealBlueprint::FLengthPrefixedJsonFraming::TryDecode(Buffer, Json, Error);
    TestTrue(TEXT("Partial payload remains buffered"), Result == CodexUnrealBlueprint::EFrameDecodeResult::NeedMoreData);
    TestEqual(TEXT("Partial frame is not consumed"), Buffer.Num(), 5);
    return true;
}

#endif
