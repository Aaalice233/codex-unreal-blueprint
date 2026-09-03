#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "PiUnrealBlueprintTestFixture.h"

#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "HAL/PlatformFilemanager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "PiUnrealBlueprintPreflight.h"
#include "PiUnrealBlueprintTypeSystem.h"
#include "PiUnrealBlueprintWritePipeline.h"
#include "UObject/Package.h"

using namespace PiUnrealBlueprint;
using namespace PiUnrealBlueprintTests;

namespace
{
    class FAddBooleanVariableOperation final : public IWriteOperation
    {
    public:
        FAddBooleanVariableOperation(UBlueprint* InBlueprint, const int32 InIndex, const FName InName,
            FString InReadOnlyFilename = FString())
            : Blueprint(InBlueprint), Index(InIndex), Name(InName), ReadOnlyFilename(MoveTemp(InReadOnlyFilename)) {}

        virtual int32 GetOperationIndex() const override { return Index; }

        virtual void GatherPreflight(FPreflightRequest& Request) const override
        {
            if (Blueprint.IsValid())
            {
                Request.TargetPackageNames.AddUnique(Blueprint->GetOutermost()->GetName());
                Request.CompilePackageNames.AddUnique(Blueprint->GetOutermost()->GetName());
            }
        }

        virtual bool Apply(FWriteMutationContext& Context, FWritePipelineError& OutError) override
        {
            if (!Blueprint.IsValid())
            {
                OutError.Code = TEXT("test.blueprintLost");
                OutError.Message = TEXT("Fixture Blueprint was collected before mutation.");
                OutError.OperationIndex = Index;
                OutError.UECallsite = TEXT("FAddBooleanVariableOperation::Apply");
                return false;
            }
            FBlueprintVariableDefinition Definition;
            Definition.Name = Name;
            Definition.Type.PinCategory = UEdGraphSchema_K2::PC_Boolean;
            Definition.DefaultValue = MakeShared<FJsonValueBoolean>(true);
            const FBlueprintOperationResult Result = FBlueprintTypeSystem::AddVariable(Blueprint.Get(), Definition, Index);
            if (!Result.bSuccess)
            {
                if (Result.Error.IsSet())
                {
                    OutError.Code = Result.Error.GetValue().Code;
                    OutError.Message = Result.Error.GetValue().Message;
                    OutError.AssetPath = Result.Error.GetValue().AssetPath;
                    OutError.OperationIndex = Result.Error.GetValue().OperationIndex;
                    OutError.UECallsite = Result.Error.GetValue().UECallsite;
                }
                return false;
            }
            Context.MarkPackageChanged(Blueprint->GetOutermost());
            if (!ReadOnlyFilename.IsEmpty()
                && !FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*ReadOnlyFilename, true))
            {
                OutError.Code = TEXT("test.readOnlyInjectionFailed");
                OutError.Message = TEXT("Could not make the fixture read-only after preflight.");
                OutError.OperationIndex = Index;
                OutError.UECallsite = TEXT("IPlatformFile::SetReadOnly");
                return false;
            }
            return true;
        }

        virtual bool VerifyInMemory(FWritePipelineError& OutError) const override
        {
            if (Blueprint.IsValid() && FBlueprintEditorUtils::FindNewVariableIndex(Blueprint.Get(), Name) != INDEX_NONE) return true;
            OutError.Code = TEXT("test.variableMissing");
            OutError.Message = TEXT("The product operation did not create the requested variable.");
            OutError.OperationIndex = Index;
            OutError.UECallsite = TEXT("FBlueprintEditorUtils::FindNewVariableIndex");
            return false;
        }

    private:
        TWeakObjectPtr<UBlueprint> Blueprint;
        int32 Index;
        FName Name;
        FString ReadOnlyFilename;
    };

    FWritePipelineRequest MakePipelineRequest(UBlueprint* Blueprint, const FString& RequestId, const FName VariableName)
    {
        FWritePipelineRequest Request;
        Request.RequestId = RequestId;
        Request.TransactionDescription = TEXT("PiUnrealBlueprint Automation fixture write");
        Request.Operations.Add(MakeShared<FAddBooleanVariableOperation>(Blueprint, 0, VariableName));
        return Request;
    }

    FWritePipelineProgress MakeProgress(TArray<FString>& Phases)
    {
        FWritePipelineProgress Progress;
        Progress.EnterPhase = [&Phases](const FString& Phase, bool bSafe, const FString& Message)
        {
            Phases.Add(Phase);
            return true;
        };
        Progress.Report = [](int32 Completed, int32 Total, const FString& Message, const FString& AssetPath) {};
        Progress.IsCancellationRequested = []() { return false; };
        Progress.Heartbeat = []() {};
        return Progress;
    }

    bool SaveCleanFixture(FScopedFixture& Fixture, UBlueprint*& Blueprint, FString& Filename)
    {
        Blueprint = Fixture.CreateBlueprint(TEXT("BP_Pipeline"));
        if (!Blueprint) return false;
        FKismetEditorUtilities::CompileBlueprint(Blueprint);
        if (!Fixture.Save(Blueprint, Filename)) return false;
        Blueprint->GetOutermost()->SetDirtyFlag(false);
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPiPipelinePersistenceIntegrationTest,
    "PiUnrealBlueprint.Integration.Pipeline.PreflightTransactionCompileSaveReload", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPiPipelinePersistenceIntegrationTest::RunTest(const FString& Parameters)
{
    FScopedFixture Fixture(TEXT("PipelineSuccess"));
    UBlueprint* Blueprint = nullptr;
    FString Filename;
    if (!TestTrue(TEXT("clean fixture saved"), SaveCleanFixture(Fixture, Blueprint, Filename))) return false;
    TArray<FString> Phases;
    FWritePipelineResult Result = FWritePipeline::Execute(
        MakePipelineRequest(Blueprint, Fixture.GetRunId() + TEXT("_write"), TEXT("PipelineFlag")), MakeProgress(Phases));
    TestTrue(TEXT("pipeline succeeds"), Result.bSucceeded);
    TestFalse(TEXT("success is not partial"), Result.bPartial);
    TestFalse(TEXT("success state is known"), Result.bStateUnknown);
    TestEqual(TEXT("all pipeline phases emitted"), FString::Join(Phases, TEXT(",")),
        FString(TEXT("preflight,modify,compile,save,reload,verify")));
    TestEqual(TEXT("one package verified"), Result.Packages.Num(), 1);
    if (Result.Packages.Num() == 1)
    {
        TestTrue(TEXT("package saved"), Result.Packages[0].bSaved);
        TestTrue(TEXT("package reloaded"), Result.Packages[0].bReloaded);
        TestTrue(TEXT("package verified"), Result.Packages[0].bVerified);
    }
    UBlueprint* Reloaded = LoadObject<UBlueprint>(nullptr, *FScopedFixture::ObjectPath(Fixture.Package(TEXT("BP_Pipeline"))));
    TestNotNull(TEXT("reloaded Blueprint is available"), Reloaded);
    if (Reloaded) TestTrue(TEXT("persisted variable survives reload"),
        FBlueprintEditorUtils::FindNewVariableIndex(Reloaded, TEXT("PipelineFlag")) != INDEX_NONE);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPiPipelinePreflightRefusalsTest,
    "PiUnrealBlueprint.FaultInjection.Pipeline.PreflightRefusals", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPiPipelinePreflightRefusalsTest::RunTest(const FString& Parameters)
{
    FScopedFixture Fixture(TEXT("PreflightRefusal"));
    UBlueprint* Blueprint = nullptr;
    FString Filename;
    if (!SaveCleanFixture(Fixture, Blueprint, Filename)) return false;

    Blueprint->GetOutermost()->SetDirtyFlag(true);
    FPreflightRequest DirtyRequest;
    DirtyRequest.TargetPackageNames.Add(Blueprint->GetOutermost()->GetName());
    FPreflightResult Dirty = FWritePreflight::Run(DirtyRequest);
    TestFalse(TEXT("dirty package rejected"), Dirty.bSucceeded);
    TestTrue(TEXT("dirty rejection is explicit"), Dirty.Issues.ContainsByPredicate([](const FPreflightIssue& Issue)
    {
        return Issue.Code == TEXT("preflight.dirtyPackage");
    }));
    Blueprint->GetOutermost()->SetDirtyFlag(false);

    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    const bool WasReadOnly = PlatformFile.IsReadOnly(*Filename);
    TestTrue(TEXT("fixture made read-only"), PlatformFile.SetReadOnly(*Filename, true));
    FPreflightRequest SourceControlRequest;
    SourceControlRequest.TargetPackageNames.Add(Blueprint->GetOutermost()->GetName());
    FPreflightResult SourceControl = FWritePreflight::Run(SourceControlRequest);
    TestFalse(TEXT("read-only/source-control refusal blocks write"), SourceControl.bSucceeded);
    TestTrue(TEXT("source-control refusal has explicit issue"), SourceControl.Issues.ContainsByPredicate([](const FPreflightIssue& Issue)
    {
        return Issue.Code == TEXT("preflight.readOnly") || Issue.Code == TEXT("preflight.sourceControlUnavailable")
            || Issue.Code == TEXT("preflight.checkedOutOther");
    }));
    PlatformFile.SetReadOnly(*Filename, WasReadOnly);

    FPreflightRequest DiskRequest;
    DiskRequest.TargetPackageNames.Add(Blueprint->GetOutermost()->GetName());
    DiskRequest.MinimumFreeSpaceReserveBytes = MAX_uint64 / 2;
    FPreflightResult Disk = FWritePreflight::Run(DiskRequest);
    TestFalse(TEXT("insufficient disk reserve rejected"), Disk.bSucceeded);
    TestTrue(TEXT("disk refusal is explicit"), Disk.Issues.ContainsByPredicate([](const FPreflightIssue& Issue)
    {
        return Issue.Code == TEXT("preflight.diskSpace");
    }));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPiPipelineCompileFailureTest,
    "PiUnrealBlueprint.FaultInjection.Pipeline.CompileFailure", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPiPipelineCompileFailureTest::RunTest(const FString& Parameters)
{
    FScopedFixture Fixture(TEXT("CompileFailure"));
    UBlueprint* Blueprint = nullptr;
    FString Filename;
    if (!SaveCleanFixture(Fixture, Blueprint, Filename)) return false;
    FWritePipelineRequest Request = MakePipelineRequest(
        Blueprint, Fixture.GetRunId() + TEXT("_compile_failure"), TEXT("CompileFailureFlag"));
    Request.BlueprintCompiler = [](UBlueprint* Target, TArray<FString>& OutMessages)
    {
        OutMessages.Add(FString::Printf(TEXT("Injected compiler failure for %s."), *Target->GetPathName()));
        return false;
    };
    TArray<FString> Phases;
    const FWritePipelineResult Result = FWritePipeline::Execute(Request, MakeProgress(Phases));
    TestFalse(TEXT("compile failure cannot report success"), Result.bSucceeded);
    TestEqual(TEXT("compile failure has stable code"), Result.Error.Code, FString(TEXT("write.compileFailed")));
    TestTrue(TEXT("compiler diagnostics are retained"), Result.Error.CompilerMessages.Num() > 0);
    TestFalse(TEXT("compile failure happens before any save"), Result.Packages.ContainsByPredicate([](const FWritePackageResult& Package)
    {
        return Package.bSaveAttempted;
    }));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPiPipelinePartialSaveFailureTest,
    "PiUnrealBlueprint.FaultInjection.Pipeline.PartialSaveFailure", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPiPipelinePartialSaveFailureTest::RunTest(const FString& Parameters)
{
    FScopedFixture Fixture(TEXT("PartialSaveFailure"));
    UBlueprint* First = Fixture.CreateBlueprint(TEXT("BP_A_First"));
    UBlueprint* Second = Fixture.CreateBlueprint(TEXT("BP_Z_Second"));
    FString FirstFilename;
    FString SecondFilename;
    if (!First || !Second || !Fixture.Save(First, FirstFilename) || !Fixture.Save(Second, SecondFilename)) return false;
    First->GetOutermost()->SetDirtyFlag(false);
    Second->GetOutermost()->SetDirtyFlag(false);

    FWritePipelineRequest Request;
    Request.RequestId = Fixture.GetRunId() + TEXT("_partial_save");
    Request.TransactionDescription = TEXT("PiUnrealBlueprint partial save fixture");
    AddExpectedError(TEXT("Error deleting file"), EAutomationExpectedErrorFlags::Contains, 1);
    Request.Operations.Add(MakeShared<FAddBooleanVariableOperation>(First, 0, TEXT("SavedBeforeFailure")));
    Request.Operations.Add(MakeShared<FAddBooleanVariableOperation>(Second, 1, TEXT("CannotSave"), SecondFilename));
    TArray<FString> Phases;
    const FWritePipelineResult Result = FWritePipeline::Execute(Request, MakeProgress(Phases));
    FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*SecondFilename, false);

    TestFalse(TEXT("partial save cannot report success"), Result.bSucceeded);
    TestEqual(TEXT("save failure has stable code"), Result.Error.Code, FString(TEXT("write.saveFailed")));
    TestTrue(TEXT("earlier saved package makes result partial"), Result.bPartial);
    TestTrue(TEXT("failed save leaves state explicitly unknown"), Result.bStateUnknown);
    TestEqual(TEXT("both impacted packages are reported"), Result.FailureReport.Assets.Num(), 2);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPiPipelinePartialUnknownManifestTest,
    "PiUnrealBlueprint.FaultInjection.Pipeline.PartialAndStateUnknownManifest", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPiPipelinePartialUnknownManifestTest::RunTest(const FString& Parameters)
{
    FScopedFixture Fixture(TEXT("PartialUnknown"));
    UBlueprint* Blueprint = nullptr;
    FString Filename;
    if (!SaveCleanFixture(Fixture, Blueprint, Filename)) return false;
    const FString PackageName = Blueprint->GetOutermost()->GetName();
    FWritePipelineRequest Request = MakePipelineRequest(Blueprint, Fixture.GetRunId() + TEXT("_partial"), TEXT("SavedBeforeHashFailure"));
    int32 HashCalls = 0;
    Request.StateHashResolver = [&HashCalls](const FString& Name, FString& OutHash, FString& OutError)
    {
        ++HashCalls;
        if (HashCalls >= 2)
        {
            OutError = TEXT("Injected post-save hash read failure.");
            return false;
        }
        return FWritePreflight::ComputePackageStateHash(Name, OutHash, OutError);
    };
    TArray<FString> Phases;
    FWritePipelineResult Result = FWritePipeline::Execute(Request, MakeProgress(Phases));
    TestFalse(TEXT("fault is not reported as success"), Result.bSucceeded);
    TestTrue(TEXT("save followed by verification fault is partial"), Result.bPartial);
    TestTrue(TEXT("post-save fault marks state unknown"), Result.bStateUnknown);
    TestEqual(TEXT("stable save failure code"), Result.Error.Code, FString(TEXT("write.saveFailed")));
    TestEqual(TEXT("failure manifest contains affected asset"), Result.FailureReport.Assets.Num(), 1);
    if (Result.FailureReport.Assets.Num() == 1)
    {
        TestEqual(TEXT("manifest package is exact"), Result.FailureReport.Assets[0].PackageName, PackageName);
        TestEqual(TEXT("unknown asset state is explicit"), Result.FailureReport.Assets[0].State, EWriteAssetState::Unknown);
        TestTrue(TEXT("manifest records save attempt"), Result.FailureReport.Assets[0].bSaveAttempted);
    }
    TestTrue(TEXT("manual recovery guidance exists"), Result.FailureReport.ManualRecoveryAdvice.Num() > 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPiPipelineCancellationBoundaryTest,
    "PiUnrealBlueprint.FaultInjection.Pipeline.ConcurrentCancellationBoundary", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPiPipelineCancellationBoundaryTest::RunTest(const FString& Parameters)
{
    FScopedFixture Fixture(TEXT("PipelineCancel"));
    UBlueprint* Blueprint = nullptr;
    FString Filename;
    if (!SaveCleanFixture(Fixture, Blueprint, Filename)) return false;
    FWritePipelineRequest Request = MakePipelineRequest(Blueprint, Fixture.GetRunId() + TEXT("_cancel"), TEXT("CancelledFlag"));
    bool bCancellationRequested = false;
    FWritePipelineProgress Progress;
    Progress.EnterPhase = [&bCancellationRequested](const FString& Phase, bool bSafe, const FString& Message)
    {
        if (Phase == TEXT("modify")) bCancellationRequested = true;
        return true;
    };
    Progress.Report = [](int32 Completed, int32 Total, const FString& Message, const FString& AssetPath) {};
    Progress.IsCancellationRequested = [&bCancellationRequested]() { return bCancellationRequested; };
    Progress.Heartbeat = []() {};
    FWritePipelineResult Result = FWritePipeline::Execute(Request, Progress);
    TestFalse(TEXT("cancelled write fails explicitly"), Result.bSucceeded);
    TestEqual(TEXT("cancellation has stable code"), Result.Error.Code, FString(TEXT("write.cancelled")));
    TestFalse(TEXT("pre-save cancellation is not partial"), Result.bPartial);
    TestFalse(TEXT("verified undo keeps state known"), Result.bStateUnknown);
    return true;
}

#endif
