#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "CodexUnrealBlueprintTestFixture.h"

#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "CodexUnrealBlueprintComponentOperations.h"
#include "CodexUnrealBlueprintEditorSafeDispatcher.h"
#include "CodexUnrealBlueprintJobs.h"
#include "CodexUnrealBlueprintProtocol.h"
#include "CodexUnrealBlueprintRequestJournal.h"
#include "CodexUnrealBlueprintService.h"

using namespace CodexUnrealBlueprint;
using namespace CodexUnrealBlueprintTests;

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodexRealMutationIdempotencyUnitTest,
    "CodexUnrealBlueprint.Unit.Idempotency.ResponseLossDoesNotRepeatMutation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexRealMutationIdempotencyUnitTest::RunTest(const FString& Parameters)
{
    FScopedFixture Fixture(TEXT("IdempotentMutation"));
    UBlueprint* Blueprint = Fixture.CreateBlueprint(TEXT("BP_Idempotent"));
    if (!TestNotNull(TEXT("Blueprint fixture created"), Blueprint)) return false;
    const FString JournalDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CodexUnrealBlueprintTests"), Fixture.GetRunId());
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
            TOptional<CodexUnrealBlueprint::FComponentReference>(), FTransform::Identity).bSuccess);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodexPublicEntryFaultMatrixTest,
    "CodexUnrealBlueprint.PublicEntry.FaultMatrix.RegistryAndContract", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexPublicEntryFaultMatrixTest::RunTest(const FString& Parameters)
{
    struct FFaultCase { const TCHAR* Name; const TCHAR* Json; EErrorCode Expected; };
    const FFaultCase Cases[] = {
        { TEXT("missing request id"),
            TEXT("{\"jsonrpc\":\"2.0\",\"id\":\"fault-1\",\"method\":\"blueprint.apply\",\"params\":{\"operations\":[{\"operation\":\"asset.create\",\"packagePath\":\"/Game/CodexAutomation/Unused\",\"kind\":\"blueprint\"}]}}"),
            EErrorCode::RequestIdRequired },
        { TEXT("unknown registry operation"),
            TEXT("{\"jsonrpc\":\"2.0\",\"id\":\"fault-2\",\"method\":\"blueprint.validate\",\"params\":{\"requestId\":\"fault-request-2\",\"operations\":[{\"operation\":\"unknown.operation\",\"assetPath\":\"/Game/Unused.Unused\"}]}}"),
            EErrorCode::UnknownOperation },
        { TEXT("wrong registry field type"),
            TEXT("{\"jsonrpc\":\"2.0\",\"id\":\"fault-3\",\"method\":\"blueprint.validate\",\"params\":{\"requestId\":\"fault-request-3\",\"operations\":[{\"operation\":\"asset.create\",\"packagePath\":42,\"kind\":\"blueprint\"}]}}"),
            EErrorCode::TypeMismatch },
        { TEXT("unknown registry field"),
            TEXT("{\"jsonrpc\":\"2.0\",\"id\":\"fault-4\",\"method\":\"blueprint.validate\",\"params\":{\"requestId\":\"fault-request-4\",\"operations\":[{\"operation\":\"asset.create\",\"packagePath\":\"/Game/CodexAutomation/Unused\",\"kind\":\"blueprint\",\"typo\":true}]}}"),
            EErrorCode::TypeMismatch },
        { TEXT("missing verification asset"),
            TEXT("{\"jsonrpc\":\"2.0\",\"id\":\"fault-5\",\"method\":\"blueprint.verify\",\"params\":{\"requestId\":\"fault-request-5\",\"assetPaths\":[\"/Game/CodexAutomation/DoesNotExist.DoesNotExist\"],\"compile\":false,\"reload\":false}}"),
            EErrorCode::AssetNotFound }
    };
    for (const FFaultCase& Case : Cases)
    {
        FProtocolRequest Request;
        FProtocolError ParseError;
        if (!TestTrue(*FString::Printf(TEXT("%s JSON parses"), Case.Name),
            FProtocolRequest::Parse(Case.Json, Request, ParseError))) continue;
        const FProtocolResponse Response = FCoreService::Get().Dispatch(Request);
        if (!Response.IsSuccess())
        {
            TestTrue(*FString::Printf(TEXT("%s returns its stable error"), Case.Name),
                Response.Error.IsSet() && Response.Error.GetValue().Code == Case.Expected);
            continue;
        }
        FString JobId;
        if (!TestTrue(*FString::Printf(TEXT("%s returns a job"), Case.Name), Response.Result->TryGetStringField(TEXT("jobId"), JobId))) continue;
        FJobSnapshot Snapshot; const double Deadline = FPlatformTime::Seconds() + 10.0;
        while (FPlatformTime::Seconds() < Deadline)
        {
            FJobManager::Get().Tick(FPlatformTime::Seconds()); FEditorSafeDispatcher::Get().Tick();
            if (FJobManager::Get().Get(JobId, Snapshot) && Snapshot.bTerminal) break;
            FPlatformProcess::Sleep(0.005f);
        }
        TestTrue(*FString::Printf(TEXT("%s returns its stable async error"), Case.Name),
            Snapshot.bTerminal && Snapshot.Error.IsSet() && Snapshot.Error.GetValue().Code == Case.Expected);
    }
    return true;
}

#endif
