#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "PiUnrealBlueprintTestFixture.h"

#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PiUnrealBlueprintCommandlet.h"
#include "PiUnrealBlueprintComponentOperations.h"
#include "PiUnrealBlueprintJobs.h"
#include "PiUnrealBlueprintProtocol.h"
#include "PiUnrealBlueprintRequestJournal.h"
#include "PiUnrealBlueprintService.h"

using namespace PiUnrealBlueprint;
using namespace PiUnrealBlueprintTests;

namespace
{
    TSharedPtr<FJsonObject> RequestParams(const FString& RequestId)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("requestId"), RequestId);
        return Params;
    }

    int32 ComponentCount(UBlueprint* Blueprint, const FString& Name)
    {
        FBlueprintOperationResult Listed = FBlueprintComponentOperations::List(Blueprint, false);
        if (!Listed.bSuccess || !Listed.Data.IsValid()) return INDEX_NONE;
        const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
        if (!Listed.Data->TryGetArrayField(TEXT("components"), Components) || !Components) return INDEX_NONE;
        int32 Count = 0;
        for (const TSharedPtr<FJsonValue>& Value : *Components)
        {
            const TSharedPtr<FJsonObject>* Component = nullptr;
            FString VariableName;
            if (Value.IsValid() && Value->TryGetObject(Component) && Component
                && (*Component)->TryGetStringField(TEXT("variableName"), VariableName) && VariableName == Name) ++Count;
        }
        return Count;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPiRealMutationIdempotencyUnitTest,
    "PiUnrealBlueprint.Unit.Idempotency.ResponseLossDoesNotRepeatMutation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPiRealMutationIdempotencyUnitTest::RunTest(const FString& Parameters)
{
    FScopedFixture Fixture(TEXT("IdempotentMutation"));
    UBlueprint* Blueprint = Fixture.CreateBlueprint(TEXT("BP_Idempotent"));
    if (!TestNotNull(TEXT("Blueprint fixture created"), Blueprint)) return false;
    const FString JournalDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("PiUnrealBlueprintTests"), Fixture.GetRunId());
    FScopedDirectory JournalCleanup(JournalDirectory);
    FRequestJournal Journal(JournalDirectory);
    FProtocolError Error;
    TestTrue(TEXT("request journal initialized"), Journal.Initialize(Error));

    const FString RequestId = Fixture.GetRunId() + TEXT("_request");
    FRequestJournalRecord Record;
    const ERequestAcceptResult Accepted = Journal.Accept(RequestId, TEXT("blueprint.apply"), RequestParams(RequestId),
        TEXT("job-original"), Record, Error);
    TestEqual(TEXT("first delivery accepted"), Accepted, ERequestAcceptResult::Accepted);
    if (Accepted == ERequestAcceptResult::Accepted)
    {
        TestTrue(TEXT("real component mutation executes once"), FBlueprintComponentOperations::Add(Blueprint,
            USceneComponent::StaticClass(), TEXT("ExactlyOnce"),
            TOptional<PiUnrealBlueprint::FComponentReference>(), FTransform::Identity).bSuccess);
        TestTrue(TEXT("journal marks request running"), Journal.MarkRunning(RequestId, Record, Error));
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetNumberField(TEXT("componentCount"), ComponentCount(Blueprint, TEXT("ExactlyOnce")));
        TestTrue(TEXT("terminal result persisted before response"), Journal.MarkTerminal(
            RequestId, Result, TOptional<FProtocolError>(), Record, Error));
    }

    FRequestJournal Reconnected(JournalDirectory);
    TestTrue(TEXT("reconnected journal initialized"), Reconnected.Initialize(Error));
    FRequestJournalRecord Replay;
    TestEqual(TEXT("lost response is replayed, not accepted again"), Reconnected.Accept(RequestId,
        TEXT("blueprint.apply"), RequestParams(RequestId), TEXT("job-must-not-run"), Replay, Error), ERequestAcceptResult::Replay);
    TestEqual(TEXT("original job identity retained"), Replay.JobId, FString(TEXT("job-original")));
    TestEqual(TEXT("real Blueprint still contains exactly one component"), ComponentCount(Blueprint, TEXT("ExactlyOnce")), 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPiPublicEntryFaultMatrixTest,
    "PiUnrealBlueprint.PublicEntry.FaultMatrix.RegistryAndContract", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPiPublicEntryFaultMatrixTest::RunTest(const FString& Parameters)
{
    struct FFaultCase { const TCHAR* Name; const TCHAR* Json; EErrorCode Expected; };
    const FFaultCase Cases[] = {
        { TEXT("missing request id"),
            TEXT("{\"jsonrpc\":\"2.0\",\"id\":\"fault-1\",\"method\":\"blueprint.apply\",\"params\":{\"operations\":[{\"operation\":\"asset.create\",\"packagePath\":\"/Game/PiAutomation/Unused\",\"kind\":\"blueprint\"}]}}"),
            EErrorCode::RequestIdRequired },
        { TEXT("unknown registry operation"),
            TEXT("{\"jsonrpc\":\"2.0\",\"id\":\"fault-2\",\"method\":\"blueprint.validate\",\"params\":{\"operations\":[{\"operation\":\"unknown.operation\",\"assetPath\":\"/Game/Unused.Unused\"}]}}"),
            EErrorCode::UnknownOperation },
        { TEXT("wrong registry field type"),
            TEXT("{\"jsonrpc\":\"2.0\",\"id\":\"fault-3\",\"method\":\"blueprint.validate\",\"params\":{\"operations\":[{\"operation\":\"asset.create\",\"packagePath\":42,\"kind\":\"blueprint\"}]}}"),
            EErrorCode::TypeMismatch },
        { TEXT("unknown registry field"),
            TEXT("{\"jsonrpc\":\"2.0\",\"id\":\"fault-4\",\"method\":\"blueprint.validate\",\"params\":{\"operations\":[{\"operation\":\"asset.create\",\"packagePath\":\"/Game/PiAutomation/Unused\",\"kind\":\"blueprint\",\"typo\":true}]}}"),
            EErrorCode::TypeMismatch },
        { TEXT("missing verification asset"),
            TEXT("{\"jsonrpc\":\"2.0\",\"id\":\"fault-5\",\"method\":\"blueprint.verify\",\"params\":{\"assetPaths\":[\"/Game/PiAutomation/DoesNotExist.DoesNotExist\"],\"compile\":false,\"reload\":false}}"),
            EErrorCode::AssetNotFound }
    };
    for (const FFaultCase& Case : Cases)
    {
        FProtocolRequest Request;
        FProtocolError ParseError;
        if (!TestTrue(*FString::Printf(TEXT("%s JSON parses"), Case.Name),
            FProtocolRequest::Parse(Case.Json, Request, ParseError))) continue;
        const FProtocolResponse Response = FCoreService::Get().Dispatch(Request);
        TestFalse(*FString::Printf(TEXT("%s cannot report success"), Case.Name), Response.IsSuccess());
        TestTrue(*FString::Printf(TEXT("%s returns its stable error"), Case.Name),
            Response.Error.IsSet() && Response.Error.GetValue().Code == Case.Expected);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPiCommandletApplyWaitsForTerminalTest,
    "PiUnrealBlueprint.Commandlet.ApplyWaitsForTerminal", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPiCommandletApplyWaitsForTerminalTest::RunTest(const FString& Parameters)
{
    FJobManager& Manager = FJobManager::Get();
    const FString JobId = Manager.Start(EJobAccess::Read, FString(),
        [](FJobExecutionContext& Context, TSharedPtr<FJsonObject>& Result, FProtocolError& Error)
        {
            Context.ReportProgress(1, 1, TEXT("commandlet terminal fixture"));
            Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("executed"), true);
            return true;
        });
    FJobSnapshot Accepted;
    if (!TestTrue(TEXT("Core job accepted"), Manager.Get(JobId, Accepted))) return false;

    FProtocolResponse Response;
    Response.Id = TEXT("commandlet-terminal-test");
    Response.Result = Accepted.ToJson();
    Response.Result->SetBoolField(TEXT("replay"), false);
    CompleteCommandletApply(TEXT("blueprint.apply"), Response);

    if (!TestTrue(TEXT("Commandlet response remains a success envelope"), Response.IsSuccess())) return false;
    TestTrue(TEXT("Commandlet waits for terminal"), Response.Result->GetBoolField(TEXT("terminal")));
    TestEqual(TEXT("Terminal phase is preserved"), Response.Result->GetStringField(TEXT("phase")), FString(TEXT("Succeeded")));
    const TSharedPtr<FJsonObject>* Result = nullptr;
    TestTrue(TEXT("Terminal Core result is inspectable"), Response.Result->TryGetObjectField(TEXT("result"), Result)
        && Result != nullptr && (*Result)->GetBoolField(TEXT("executed")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPiCoreDispatchParityFixtureTest,
    "PiUnrealBlueprint.PublicEntry.Parity.ExportCoreStatus", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPiCoreDispatchParityFixtureTest::RunTest(const FString& Parameters)
{
    const FString ResultPath = FPlatformMisc::GetEnvironmentVariable(TEXT("PI_UE_PARITY_DIRECT_RESULT"));
    if (ResultPath.IsEmpty())
    {
        AddWarning(TEXT("PI_UE_PARITY_DIRECT_RESULT is unset; commandlet parity is owned by tests/fixtures/driver."));
        return true;
    }

    const FString Id = TEXT("commandlet-parity-capabilities");
    const FString RequestJson = FString::Printf(
        TEXT("{\"jsonrpc\":\"2.0\",\"id\":\"%s\",\"method\":\"blueprint.capabilities\",\"params\":{\"operationNames\":[\"asset.create\",\"component.add\",\"graph.add\",\"widget.add\",\"anim.variable.add\"]}}"), *Id);
    FProtocolRequest Request;
    FProtocolError Error;
    if (!TestTrue(TEXT("parity request parses"), FProtocolRequest::Parse(RequestJson, Request, Error))) return false;
    const FProtocolResponse Direct = FCoreService::Get().Dispatch(Request);
    if (!TestTrue(TEXT("core public entry succeeds"), Direct.IsSuccess())) return false;
    TestTrue(TEXT("direct parity result is exported"), FFileHelper::SaveStringToFile(
        Direct.ToJsonString(), *ResultPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
    return true;
}

#endif
