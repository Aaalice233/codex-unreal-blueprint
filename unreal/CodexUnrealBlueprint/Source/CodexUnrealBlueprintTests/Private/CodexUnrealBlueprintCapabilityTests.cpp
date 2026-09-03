#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "CodexUnrealBlueprintTestFixture.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimMontage.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Async/TaskGraphInterfaces.h"
#include "Components/PanelWidget.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextBlock.h"
#include "Components/CanvasPanel.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Engine/UserDefinedEnum.h"
#include "Engine/UserDefinedStruct.h"
#include "Materials/Material.h"
#include "Misc/PackageName.h"
#include "NiagaraSystem.h"
#include "UObject/Package.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "CodexUnrealBlueprintActionCatalog.h"
#include "CodexUnrealAssetInspection.h"
#include "CodexUnrealBlueprintComponentOperations.h"
#include "CodexUnrealBlueprintEditorSafeDispatcher.h"
#include "CodexUnrealBlueprintGraphOperations.h"
#include "CodexUnrealBlueprintJobs.h"
#include "CodexUnrealBlueprintOperationRegistry.h"
#include "CodexUnrealBlueprintProtocol.h"
#include "CodexUnrealBlueprintService.h"
#include "CodexUnrealBlueprintTypeSystem.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Animation/WidgetAnimation.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"

using namespace CodexUnrealBlueprint;
using namespace CodexUnrealBlueprintTests;

namespace
{
    template <typename TAsset>
    TAsset* CreateInspectableAsset(FScopedFixture& Fixture, const FString& Leaf)
    {
        const FString PackageName = Fixture.Package(Leaf);
        UPackage* Package = CreatePackage(*PackageName);
        TAsset* Asset = NewObject<TAsset>(Package, FName(*FPackageName::GetLongPackageAssetName(PackageName)),
            RF_Public | RF_Standalone | RF_Transactional);
        if (Asset)
        {
            FAssetRegistryModule::AssetCreated(Asset);
            Package->MarkPackageDirty();
        }
        return Asset;
    }

    FEdGraphPinType PinType(const FName Category)
    {
        FEdGraphPinType Type;
        Type.PinCategory = Category;
        return Type;
    }

    CodexUnrealBlueprint::FComponentReference ComponentRef(const FString& Name)
    {
        CodexUnrealBlueprint::FComponentReference Reference;
        Reference.VariableName = FName(*Name);
        return Reference;
    }

    UEdGraph* FindGraph(UBlueprint* Blueprint, const FString& Name)
    {
        TArray<UEdGraph*> Graphs;
        Blueprint->GetAllGraphs(Graphs);
        for (UEdGraph* Graph : Graphs) if (Graph && Graph->GetName() == Name) return Graph;
        return nullptr;
    }

    int32 CountComponents(UBlueprint* Blueprint, const FString& Name)
    {
        const FBlueprintOperationResult Listed = FBlueprintComponentOperations::List(Blueprint, false);
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

    bool DispatchOperations(const FString& RequestId, const TArray<TSharedRef<FJsonObject>>& Operations,
        FJobSnapshot& OutSnapshot, FString& OutError)
    {
        TArray<TSharedPtr<FJsonValue>> OperationValues;
        for (const TSharedRef<FJsonObject>& Operation : Operations)
            OperationValues.Add(MakeShared<FJsonValueObject>(Operation));
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("requestId"), RequestId);
        Params->SetArrayField(TEXT("operations"), OperationValues);
        TSharedRef<FJsonObject> Envelope = MakeShared<FJsonObject>();
        Envelope->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
        Envelope->SetStringField(TEXT("id"), RequestId);
        Envelope->SetStringField(TEXT("method"), TEXT("blueprint.apply"));
        Envelope->SetObjectField(TEXT("params"), Params);
        FString Json;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
        if (!FJsonSerializer::Serialize(Envelope, Writer))
        {
            OutError = TEXT("Failed to serialize the public JSON request.");
            return false;
        }
        FProtocolRequest Request;
        FProtocolError ParseError;
        if (!FProtocolRequest::Parse(Json, Request, ParseError))
        {
            OutError = ParseError.Message;
            return false;
        }
        const FProtocolResponse Response = FCoreService::Get().Dispatch(Request);
        if (!Response.IsSuccess() || !Response.Result.IsValid())
        {
            OutError = Response.Error.IsSet() ? Response.Error.GetValue().Message : TEXT("Dispatch returned no result.");
            return false;
        }
        FString JobId;
        if (!Response.Result->TryGetStringField(TEXT("jobId"), JobId))
        {
            OutError = TEXT("Dispatch did not return jobId.");
            return false;
        }
        const double Deadline = FPlatformTime::Seconds() + 30.0;
        while (FPlatformTime::Seconds() < Deadline)
        {
            FJobManager::Get().Tick(FPlatformTime::Seconds());
            FEditorSafeDispatcher::Get().Tick();
            FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
            if (FJobManager::Get().Get(JobId, OutSnapshot) && OutSnapshot.bTerminal)
            {
                if (OutSnapshot.Phase == EJobPhase::Succeeded) return true;
                OutError = OutSnapshot.Error.IsSet() ? OutSnapshot.Error.GetValue().Message
                    : FString::Printf(TEXT("Job ended in phase %s."), LexToString(OutSnapshot.Phase));
                return false;
            }
            FPlatformProcess::Sleep(0.005f);
        }
        OutError = TEXT("Timed out waiting for the public write job.");
        return false;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodexLayeredAssetInspectionUnitTest,
    "CodexUnrealBlueprint.Unit.Assets.LayeredInspection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexLayeredAssetInspectionUnitTest::RunTest(const FString& Parameters)
{
    FScopedFixture Fixture(TEXT("LayeredAssets"));
    UBlueprint* Blueprint = Fixture.CreateBlueprint(TEXT("BP_Inspectable"));
    UWidgetBlueprint* Widget = Fixture.CreateWidgetBlueprint(TEXT("WBP_Inspectable"));
    UMaterial* Material = CreateInspectableAsset<UMaterial>(Fixture, TEXT("M_Inspectable"));
    UAnimMontage* Montage = CreateInspectableAsset<UAnimMontage>(Fixture, TEXT("AM_Inspectable"));
    UNiagaraSystem* Niagara = CreateInspectableAsset<UNiagaraSystem>(Fixture, TEXT("NS_Inspectable"));
    if (!TestNotNull(TEXT("Blueprint fixture exists"), Blueprint)
        || !TestNotNull(TEXT("Widget Blueprint fixture exists"), Widget)
        || !TestNotNull(TEXT("Material fixture exists"), Material)
        || !TestNotNull(TEXT("Montage fixture exists"), Montage)
        || !TestNotNull(TEXT("Niagara fixture exists"), Niagara)) return false;

    struct FCase { UObject* Asset; const TCHAR* SpecializedField; bool bEditable; };
    const FCase Cases[] = {
        {Blueprint, TEXT("blueprint"), true},
        {Widget, TEXT("blueprint"), true},
        {Material, TEXT("material"), false},
        {Montage, TEXT("animMontage"), false},
        {Niagara, TEXT("niagaraSystem"), false}
    };
    for (const FCase& Case : Cases)
    {
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>(); FProtocolError Error;
        if (!TestTrue(*FString::Printf(TEXT("%s layered inspection succeeds"), *Case.Asset->GetName()),
            FUnrealAssetInspection::Inspect(Case.Asset->GetPathName(), {}, {}, 0, 500, Result, Error))) continue;
        const TSharedPtr<FJsonObject>* Facets = nullptr; const TSharedPtr<FJsonObject>* Support = nullptr;
        const TSharedPtr<FJsonObject>* Specialized = nullptr;
        Result->TryGetObjectField(TEXT("facets"), Facets);
        if (!TestTrue(TEXT("facets returned"), Facets != nullptr)) continue;
        (*Facets)->TryGetObjectField(TEXT("support"), Support);
        (*Facets)->TryGetObjectField(TEXT("specialized"), Specialized);
        TestTrue(TEXT("generic layer is declared"), Support && (*Support)->GetBoolField(TEXT("generic")));
        TestEqual(TEXT("editable layer is accurate"), Support ? (*Support)->GetBoolField(TEXT("editable")) : false, Case.bEditable);
        TestTrue(TEXT("specialized snapshot matches asset family"), Specialized && (*Specialized)->HasField(Case.SpecializedField));
    }

    TSharedRef<FJsonObject> Comparison = MakeShared<FJsonObject>(); FProtocolError CompareError;
    TestTrue(TEXT("same asset comparison succeeds"), FUnrealAssetInspection::Compare(
        Material->GetPathName(), Material->GetPathName(), {}, {}, 0, 500, Comparison, CompareError));
    TestTrue(TEXT("same asset is identical"), Comparison->GetBoolField(TEXT("identical")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodexAssetCrudAndKindsUnitTest,
    "CodexUnrealBlueprint.Unit.Assets.CrudKindsStructEnumLevel", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexAssetCrudAndKindsUnitTest::RunTest(const FString& Parameters)
{
    FScopedFixture Fixture(TEXT("AssetCrud"));
    UBlueprint* Blueprint = Fixture.CreateBlueprint(TEXT("BP_Source"));
    TestNotNull(TEXT("normal Blueprint created by low-level asset operation"), Blueprint);
    if (!Blueprint) return false;

    FBlueprintOperationResult Duplicate = FBlueprintAssetOperations::Duplicate(Blueprint, Fixture.Package(TEXT("BP_Copy")));
    TestTrue(TEXT("duplicate succeeds"), Duplicate.bSuccess);
    UObject* Copy = LoadObject<UObject>(nullptr, *FScopedFixture::ObjectPath(Fixture.Package(TEXT("BP_Copy"))));
    TestNotNull(TEXT("duplicate exists"), Copy);
    FBlueprintOperationResult Rename = FBlueprintAssetOperations::RenameOrMove(Copy, Fixture.Package(TEXT("Moved/BP_Renamed")));
    TestTrue(TEXT("rename and move succeeds"), Rename.bSuccess);

    TArray<TPair<EBlueprintAssetKind, FString>> Kinds;
    Kinds.Add(TPair<EBlueprintAssetKind, FString>(EBlueprintAssetKind::Interface, TEXT("BPI_Test")));
    Kinds.Add(TPair<EBlueprintAssetKind, FString>(EBlueprintAssetKind::FunctionLibrary, TEXT("BFL_Test")));
    Kinds.Add(TPair<EBlueprintAssetKind, FString>(EBlueprintAssetKind::MacroLibrary, TEXT("BML_Test")));
    for (const TPair<EBlueprintAssetKind, FString>& Kind : Kinds)
    {
        FBlueprintOperationResult Created = FBlueprintAssetOperations::Create(Fixture.Package(Kind.Value), Kind.Key, UObject::StaticClass());
        TestTrue(*FString::Printf(TEXT("asset kind %s creates"), *Kind.Value), Created.bSuccess);
    }

    UUserDefinedStruct* Struct = Fixture.CreateStruct(TEXT("S_Test"));
    TestNotNull(TEXT("struct created"), Struct);
    if (Struct)
    {
        FBlueprintOperationResult AddField = FBlueprintAssetOperations::AddStructField(
            Struct, TEXT("Count"), PinType(UEdGraphSchema_K2::PC_Int), TEXT("3"), TEXT("fixture field"));
        TestTrue(TEXT("struct field added"), AddField.bSuccess);
        FString GuidText;
        AddField.Data->TryGetStringField(TEXT("fieldGuid"), GuidText);
        FGuid FieldGuid;
        FGuid::Parse(GuidText, FieldGuid);
        TestTrue(TEXT("struct field updated"), FBlueprintAssetOperations::UpdateStructField(
            Struct, FieldGuid, TEXT("Total"), PinType(UEdGraphSchema_K2::PC_Int), TEXT("7"), TEXT("updated")).bSuccess);
        TestTrue(TEXT("struct field removed"), FBlueprintAssetOperations::RemoveStructField(Struct, FieldGuid).bSuccess);
    }

    UUserDefinedEnum* Enum = Fixture.CreateEnum(TEXT("E_Test"));
    TestNotNull(TEXT("enum created"), Enum);
    if (Enum)
    {
        FBlueprintOperationResult AddValue = FBlueprintAssetOperations::AddEnumValue(Enum, TEXT("Ready"));
        TestTrue(TEXT("enum value added"), AddValue.bSuccess);
        const int32 ValueIndex = AddValue.Data.IsValid() ? static_cast<int32>(AddValue.Data->GetNumberField(TEXT("valueIndex"))) : INDEX_NONE;
        TestTrue(TEXT("enum value renamed"), FBlueprintAssetOperations::RenameEnumValue(Enum, ValueIndex, TEXT("Running")).bSuccess);
        TestTrue(TEXT("enum bitflags enabled"), FBlueprintAssetOperations::SetEnumBitflags(Enum, true).bSuccess);
        TestTrue(TEXT("enum value removed"), FBlueprintAssetOperations::RemoveEnumValue(Enum, ValueIndex).bSuccess);
    }

    UWorld* World = Fixture.CreateWorld(TEXT("L_Test"));
    TestNotNull(TEXT("fixture world created under sandbox"), World);
    if (World) TestTrue(TEXT("level Blueprint low-level operation succeeds"), FBlueprintAssetOperations::GetOrCreateLevelBlueprint(World).bSuccess);

    UObject* Renamed = LoadObject<UObject>(nullptr, *FScopedFixture::ObjectPath(Fixture.Package(TEXT("Moved/BP_Renamed"))));
    if (Renamed) TestTrue(TEXT("renamed copy deleted"), FBlueprintAssetOperations::Delete(Renamed).bSuccess);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodexTypesDefaultsAndComponentsUnitTest,
    "CodexUnrealBlueprint.Unit.Blueprint.TypesDefaultsComponents", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexTypesDefaultsAndComponentsUnitTest::RunTest(const FString& Parameters)
{
    FScopedFixture Fixture(TEXT("TypesComponents"));
    UBlueprint* Blueprint = Fixture.CreateBlueprint(TEXT("BP_Types"));
    TestNotNull(TEXT("Blueprint fixture created"), Blueprint);
    if (!Blueprint) return false;

    FBlueprintVariableDefinition Count;
    Count.Name = TEXT("Count");
    Count.Type = PinType(UEdGraphSchema_K2::PC_Int);
    Count.DefaultValue = MakeShared<FJsonValueNumber>(5);
    Count.Category = TEXT("Automation");
    Count.Tooltip = TEXT("Unit integer");
    Count.bInstanceEditable = true;
    TestTrue(TEXT("typed variable added"), FBlueprintTypeSystem::AddVariable(Blueprint, Count).bSuccess);
    FKismetEditorUtilities::CompileBlueprint(Blueprint);
    TestTrue(TEXT("class default set through reflected type system"), FBlueprintAssetOperations::SetClassDefault(
        Blueprint, TEXT("Count"), MakeShared<FJsonValueNumber>(17)).bSuccess);
    FBlueprintOperationResult Defaults = FBlueprintAssetOperations::ReadClassDefaults(Blueprint, {TEXT("Count")});
    TestTrue(TEXT("class default read succeeds"), Defaults.bSuccess);
    if (Defaults.bSuccess)
    {
        const TSharedPtr<FJsonObject>* Values = nullptr;
        Defaults.Data->TryGetObjectField(TEXT("values"), Values);
        TestEqual(TEXT("default round trips"), static_cast<int32>((*Values)->GetNumberField(TEXT("Count"))), 17);
    }

    const FTransform RootTransform(FRotator::ZeroRotator, FVector(1, 2, 3), FVector::OneVector);
    FBlueprintOperationResult Root = FBlueprintComponentOperations::Add(
        Blueprint, USceneComponent::StaticClass(), TEXT("FixtureRoot"),
        TOptional<CodexUnrealBlueprint::FComponentReference>(), RootTransform);
    TestTrue(TEXT("root scene component added"), Root.bSuccess);
    FBlueprintOperationResult Mesh = FBlueprintComponentOperations::Add(
        Blueprint, UStaticMeshComponent::StaticClass(), TEXT("FixtureMesh"), ComponentRef(TEXT("FixtureRoot")), FTransform::Identity);
    TestTrue(TEXT("child component added"), Mesh.bSuccess);
    TestTrue(TEXT("component list includes local hierarchy"), FBlueprintComponentOperations::List(Blueprint, true).bSuccess);
    TestTrue(TEXT("component transform set"), FBlueprintComponentOperations::SetTransform(
        Blueprint, ComponentRef(TEXT("FixtureMesh")), FTransform(FRotator(0, 45, 0), FVector(10, 0, 0))).bSuccess);
    TestTrue(TEXT("component property set"), FBlueprintComponentOperations::SetProperty(
        Blueprint, ComponentRef(TEXT("FixtureMesh")), TEXT("bVisible"), MakeShared<FJsonValueBoolean>(false), false).bSuccess);
    TestTrue(TEXT("component renamed"), FBlueprintComponentOperations::Rename(
        Blueprint, ComponentRef(TEXT("FixtureMesh")), TEXT("VisualMesh")).bSuccess);
    TestTrue(TEXT("component made root"), FBlueprintComponentOperations::SetRoot(Blueprint, ComponentRef(TEXT("FixtureRoot"))).bSuccess);
    TestTrue(TEXT("component removed"), FBlueprintComponentOperations::Remove(
        Blueprint, ComponentRef(TEXT("VisualMesh")), false).bSuccess);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodexClassDefaultPreflightScopeUnitTest,
    "CodexUnrealBlueprint.Unit.Blueprint.ClassDefaultPreflightScope", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexClassDefaultPreflightScopeUnitTest::RunTest(const FString& Parameters)
{
    FScopedFixture Fixture(TEXT("ClassDefaultPreflightScope"));
    UBlueprint* Target = Fixture.CreateBlueprint(TEXT("BP_Target"));
    if (!TestNotNull(TEXT("target Blueprint created"), Target)) return false;

    FBlueprintVariableDefinition Count;
    Count.Name = TEXT("Count");
    Count.Type = PinType(UEdGraphSchema_K2::PC_Int);
    Count.DefaultValue = MakeShared<FJsonValueNumber>(1);
    if (!TestTrue(TEXT("target class-default variable added"), FBlueprintTypeSystem::AddVariable(Target, Count).bSuccess))
        return false;
    FKismetEditorUtilities::CompileBlueprint(Target);
    FString TargetFilename;
    if (!TestTrue(TEXT("target Blueprint saved"), Fixture.Save(Target, TargetFilename))) return false;
    Target->GetOutermost()->SetDirtyFlag(false);

    UBlueprint* Referencer = Fixture.CreateBlueprint(TEXT("BP_Referencer"), Target->GeneratedClass);
    if (!TestNotNull(TEXT("referencing Blueprint created"), Referencer)) return false;
    FKismetEditorUtilities::CompileBlueprint(Referencer);
    FString ReferencerFilename;
    if (!TestTrue(TEXT("referencing Blueprint saved"), Fixture.Save(Referencer, ReferencerFilename))) return false;
    Referencer->GetOutermost()->SetDirtyFlag(false);

    IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    Registry.ScanModifiedAssetFiles({TargetFilename, ReferencerFilename});
    TArray<FName> Referencers;
    Registry.GetReferencers(Target->GetOutermost()->GetFName(), Referencers,
        UE::AssetRegistry::EDependencyCategory::Package);
    if (!TestTrue(TEXT("fixture exposes a real package referencer"),
        Referencers.Contains(Referencer->GetOutermost()->GetFName()))) return false;

    TSharedRef<FJsonObject> Operation = FScopedFixture::Operation(TEXT("asset.classDefault.set"));
    Operation->SetStringField(TEXT("assetPath"), Target->GetPathName());
    Operation->SetStringField(TEXT("propertyPath"), TEXT("Count"));
    Operation->SetNumberField(TEXT("value"), 2);
    FPreflightRequest Preflight;
    TSharedRef<FJsonObject> Validation = MakeShared<FJsonObject>();
    FProtocolError Error;
    if (!TestTrue(TEXT("class-default operation validates"),
        FOperationRegistry::Get().Validate({Operation}, Preflight, Validation, Error))) return false;
    TestEqual(TEXT("class-default preflight only includes the modified package"),
        Preflight.TargetPackageNames.Num(), 1);
    TestTrue(TEXT("class-default preflight includes its target package"),
        Preflight.TargetPackageNames.Contains(Target->GetOutermost()->GetName()));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodexGraphAndActionCatalogUnitTest,
    "CodexUnrealBlueprint.Unit.Graph.ActionCatalogAndSchema", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexGraphAndActionCatalogUnitTest::RunTest(const FString& Parameters)
{
    FScopedFixture Fixture(TEXT("Graph"));
    UBlueprint* Blueprint = Fixture.CreateBlueprint(TEXT("BP_Graph"));
    if (!TestNotNull(TEXT("Blueprint fixture created"), Blueprint)) return false;

    TSharedRef<FJsonObject> AddGraph = FScopedFixture::Operation(TEXT("graph.add"));
    AddGraph->SetStringField(TEXT("name"), TEXT("Calculate"));
    AddGraph->SetStringField(TEXT("kind"), TEXT("function"));
    FGraphOperationResult GraphResult;
    FGraphActionError Error;
    TestTrue(TEXT("function graph added through graph operation entry"),
        FBlueprintGraphOperations::Apply(Blueprint, AddGraph, GraphResult, Error));
    UEdGraph* Graph = FindGraph(Blueprint, TEXT("Calculate"));
    if (!TestNotNull(TEXT("added graph found"), Graph)) return false;

    TSharedRef<FJsonObject> Comment = FScopedFixture::Operation(TEXT("node.comment"));
    Comment->SetStringField(TEXT("graphName"), TEXT("Calculate"));
    Comment->SetNumberField(TEXT("x"), 100);
    Comment->SetNumberField(TEXT("y"), 200);
    Comment->SetStringField(TEXT("text"), TEXT("Automation"));
    TestTrue(TEXT("comment node created"), FBlueprintGraphOperations::Apply(Blueprint, Comment, GraphResult, Error));

    FGraphActionSearchRequest Search;
    Search.Query = TEXT("Print String");
    Search.Blueprint = Blueprint;
    Search.Graph = Graph;
    Search.PageSize = 100;
    FGraphActionSearchResult SearchResult;
    TestTrue(TEXT("action catalog search succeeds"), FBlueprintGraphActionCatalog::Search(Search, SearchResult, Error));
    TestTrue(TEXT("catalog returns compatible actions"), SearchResult.Actions.Num() > 0);
    if (SearchResult.Actions.Num() > 0)
    {
        const TSharedPtr<FJsonObject>* Action = nullptr;
        if (SearchResult.Actions[0]->TryGetObject(Action) && Action)
        {
            const FString ActionId = (*Action)->GetStringField(TEXT("actionId"));
            UBlueprintNodeSpawner* Spawner = nullptr;
            TestTrue(TEXT("catalog action resolves in exact context"),
                FBlueprintGraphActionCatalog::Resolve(ActionId, Blueprint, Graph, Spawner, Error));
            FString StaleId = ActionId;
            StaleId += TEXT("stale");
            TestFalse(TEXT("stale action id is rejected"),
                FBlueprintGraphActionCatalog::Resolve(StaleId, Blueprint, Graph, Spawner, Error));
        }
    }

    TSharedRef<FJsonObject> Invalid = FScopedFixture::Operation(TEXT("node.comment"));
    Invalid->SetStringField(TEXT("graphName"), TEXT("Calculate"));
    Invalid->SetStringField(TEXT("x"), TEXT("wrong-type"));
    Invalid->SetNumberField(TEXT("y"), 0);
    TestFalse(TEXT("wrong parameter type is not coerced"), FBlueprintGraphOperations::Apply(Blueprint, Invalid, GraphResult, Error));
    TestFalse(TEXT("graph error contains stable code"), Error.Code.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodexPublicAssetFamilyE2ETest,
    "CodexUnrealBlueprint.E2E.PublicEntry.AssetFamily", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexPublicAssetFamilyE2ETest::RunTest(const FString& Parameters)
{
    FScopedFixture Fixture(TEXT("PublicAssetFamily"));
    struct FAssetCase { const TCHAR* Leaf; const TCHAR* Kind; UClass* ExpectedClass; };
    const FAssetCase Cases[] = {
        { TEXT("BP_Public"), TEXT("blueprint"), UBlueprint::StaticClass() },
        { TEXT("BPI_Public"), TEXT("interface"), UBlueprint::StaticClass() },
        { TEXT("BFL_Public"), TEXT("functionLibrary"), UBlueprint::StaticClass() },
        { TEXT("BML_Public"), TEXT("macroLibrary"), UBlueprint::StaticClass() },
        { TEXT("S_Public"), TEXT("struct"), UUserDefinedStruct::StaticClass() },
        { TEXT("E_Public"), TEXT("enum"), UUserDefinedEnum::StaticClass() }
    };
    UWorld* World = Fixture.CreateWorld(TEXT("L_Public"));
    ULevelScriptBlueprint* LevelBlueprint = World && World->PersistentLevel
        ? World->PersistentLevel->GetLevelScriptBlueprint() : nullptr;
    FString WorldFilename;
    if (!TestNotNull(TEXT("Level Blueprint fixture created"), LevelBlueprint)
        || !TestTrue(TEXT("clean level fixture saved"), Fixture.Save(World, WorldFilename))) return false;
    World->GetOutermost()->SetDirtyFlag(false);
    const FString LevelBlueprintPath = LevelBlueprint->GetPathName();

    TArray<TSharedRef<FJsonObject>> Operations;
    for (const FAssetCase& Case : Cases)
    {
        TSharedRef<FJsonObject> Operation = FScopedFixture::Operation(TEXT("asset.create"));
        Operation->SetStringField(TEXT("packagePath"), Fixture.Package(Case.Leaf));
        Operation->SetStringField(TEXT("kind"), Case.Kind);
        if (FCString::Strcmp(Case.Kind, TEXT("blueprint")) == 0)
            Operation->SetStringField(TEXT("parentClassPath"), AActor::StaticClass()->GetPathName());
        Operations.Add(Operation);
    }
    const FString BlueprintPath = FScopedFixture::ObjectPath(Fixture.Package(TEXT("BP_Public")));
    TSharedRef<FJsonObject> Variable = FScopedFixture::Operation(TEXT("variable.add"));
    Variable->SetStringField(TEXT("assetPath"), BlueprintPath);
    Variable->SetStringField(TEXT("name"), TEXT("PublicCount"));
    TSharedRef<FJsonObject> IntegerType = MakeShared<FJsonObject>();
    IntegerType->SetStringField(TEXT("category"), UEdGraphSchema_K2::PC_Int.ToString());
    Variable->SetObjectField(TEXT("type"), IntegerType);
    Variable->SetNumberField(TEXT("default"), 11);
    Operations.Add(Variable);
    TSharedRef<FJsonObject> Component = FScopedFixture::Operation(TEXT("component.add"));
    Component->SetStringField(TEXT("assetPath"), BlueprintPath);
    Component->SetStringField(TEXT("classPath"), USceneComponent::StaticClass()->GetPathName());
    Component->SetStringField(TEXT("variableName"), TEXT("PublicRoot"));
    Operations.Add(Component);
    TSharedRef<FJsonObject> Graph = FScopedFixture::Operation(TEXT("graph.add"));
    Graph->SetStringField(TEXT("assetPath"), BlueprintPath);
    Graph->SetStringField(TEXT("kind"), TEXT("function"));
    Graph->SetStringField(TEXT("name"), TEXT("PublicFunction"));
    Operations.Add(Graph);
    TSharedRef<FJsonObject> LevelGraph = FScopedFixture::Operation(TEXT("graph.add"));
    LevelGraph->SetStringField(TEXT("assetPath"), LevelBlueprintPath);
    LevelGraph->SetStringField(TEXT("kind"), TEXT("levelScript"));
    LevelGraph->SetStringField(TEXT("name"), TEXT("PublicLevelGraph"));
    Operations.Add(LevelGraph);
    FJobSnapshot Snapshot;
    FString Error;
    if (!TestTrue(TEXT("asset family writes through Core Dispatch and OperationRegistry"),
        DispatchOperations(Fixture.GetRunId() + TEXT("_assets"), Operations, Snapshot, Error)))
    {
        AddError(Error);
        return false;
    }
    for (const FAssetCase& Case : Cases)
    {
        UObject* Asset = LoadObject<UObject>(nullptr, *FScopedFixture::ObjectPath(Fixture.Package(Case.Leaf)));
        TestNotNull(*FString::Printf(TEXT("%s persists after public pipeline reload"), Case.Leaf), Asset);
        if (Asset)
        {
            TestTrue(*FString::Printf(TEXT("%s has the expected asset family"), Case.Leaf),
                Asset->IsA(Case.ExpectedClass));
            TestFalse(*FString::Printf(TEXT("%s remains reloadable after Asset Registry synchronization"), Case.Leaf),
                Asset->GetOutermost()->HasAnyPackageFlags(PKG_InMemoryOnly));
        }
    }
    UBlueprint* PublicBlueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
    if (TestNotNull(TEXT("public Blueprint reloads for independent assertions"), PublicBlueprint))
    {
        TestTrue(TEXT("public type operation persists"),
            FBlueprintEditorUtils::FindNewVariableIndex(PublicBlueprint, TEXT("PublicCount")) != INDEX_NONE);
        TestEqual(TEXT("public component operation persists"), CountComponents(PublicBlueprint, TEXT("PublicRoot")), 1);
        TestNotNull(TEXT("public graph operation persists"), FindGraph(PublicBlueprint, TEXT("PublicFunction")));
    }
    ULevelScriptBlueprint* ReloadedLevelBlueprint = LoadObject<ULevelScriptBlueprint>(nullptr, *LevelBlueprintPath);
    TestNotNull(TEXT("Level Blueprint persists after public pipeline reload"), ReloadedLevelBlueprint);
    if (ReloadedLevelBlueprint)
        TestNotNull(TEXT("public Level Blueprint graph persists"), FindGraph(ReloadedLevelBlueprint, TEXT("PublicLevelGraph")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodexPublicInspectionLevelAndValidationTest,
    "CodexUnrealBlueprint.PublicEntry.InspectLevelAndValidatePreflight", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexPublicInspectionLevelAndValidationTest::RunTest(const FString& Parameters)
{
    FScopedFixture Fixture(TEXT("PublicReadValidate"));
    UBlueprint* Blueprint = Fixture.CreateBlueprint(TEXT("BP_Defaults"));
    if (!TestNotNull(TEXT("Blueprint fixture created"), Blueprint)) return false;
    FBlueprintVariableDefinition Count;
    Count.Name = TEXT("Count");
    Count.Type = PinType(UEdGraphSchema_K2::PC_Int);
    Count.DefaultValue = MakeShared<FJsonValueNumber>(5);
    if (!TestTrue(TEXT("default fixture variable added"), FBlueprintTypeSystem::AddVariable(Blueprint, Count).bSuccess)) return false;
    FKismetEditorUtilities::CompileBlueprint(Blueprint);
    if (!TestTrue(TEXT("class default fixture value set"), FBlueprintAssetOperations::SetClassDefault(
        Blueprint, TEXT("Count"), MakeShared<FJsonValueNumber>(29)).bSuccess)) return false;

    FProtocolRequest InspectDefaults;
    InspectDefaults.Id = TEXT("inspect-defaults");
    InspectDefaults.Params = MakeShared<FJsonObject>();
    InspectDefaults.Method = TEXT("blueprint.inspect");
    InspectDefaults.Params->SetStringField(TEXT("assetPath"), Blueprint->GetPathName());
    InspectDefaults.Params->SetArrayField(TEXT("facets"), {MakeShared<FJsonValueString>(TEXT("classDefaults"))});
    InspectDefaults.Params->SetArrayField(TEXT("classDefaultPropertyPaths"), {
        MakeShared<FJsonValueString>(TEXT("Count"))});
    const FProtocolResponse DefaultsResponse = FCoreService::Get().Dispatch(InspectDefaults);
    if (!TestTrue(TEXT("class defaults are reachable through Core Dispatch"), DefaultsResponse.IsSuccess())) return false;
    const TSharedPtr<FJsonObject>* Facets = nullptr;
    const TSharedPtr<FJsonObject>* Defaults = nullptr;
    const TSharedPtr<FJsonObject>* Values = nullptr;
    if (!TestTrue(TEXT("classDefaults facet is structured"), DefaultsResponse.Result->TryGetObjectField(TEXT("facets"), Facets)
        && Facets && (*Facets)->TryGetObjectField(TEXT("classDefaults"), Defaults) && Defaults
        && (*Defaults)->TryGetObjectField(TEXT("values"), Values) && Values)) return false;
    TestEqual(TEXT("class default value is returned"), static_cast<int32>((*Values)->GetNumberField(TEXT("Count"))), 29);

    UWorld* World = Fixture.CreateWorld(TEXT("L_PublicLevel"));
    FString WorldFilename;
    if (!TestNotNull(TEXT("Map fixture created"), World)
        || !TestTrue(TEXT("Map fixture saved before public apply"), Fixture.Save(World, WorldFilename))) return false;
    World->GetOutermost()->SetDirtyFlag(false);
    const FString WorldPath = World->GetPathName();
    TSharedRef<FJsonObject> LevelOperation = FScopedFixture::Operation(TEXT("asset.levelBlueprint.getOrCreate"));
    LevelOperation->SetStringField(TEXT("assetPath"), WorldPath);
    FJobSnapshot LevelSnapshot;
    FString ApplyError;
    if (!TestTrue(TEXT("Level Blueprint get/create is reachable through generic apply"),
        DispatchOperations(Fixture.GetRunId() + TEXT("_level"), {LevelOperation}, LevelSnapshot, ApplyError)))
    {
        AddError(ApplyError);
        return false;
    }

    FProtocolRequest InspectLevel;
    InspectLevel.Id = TEXT("inspect-level");
    InspectLevel.Method = TEXT("blueprint.inspect");
    InspectLevel.Params = MakeShared<FJsonObject>();
    InspectLevel.Params->SetStringField(TEXT("assetPath"), WorldPath);
    InspectLevel.Params->SetArrayField(TEXT("facets"), {MakeShared<FJsonValueString>(TEXT("levelBlueprint"))});
    const FProtocolResponse LevelResponse = FCoreService::Get().Dispatch(InspectLevel);
    if (!TestTrue(TEXT("Map Level Blueprint is reachable through generic inspect"), LevelResponse.IsSuccess())) return false;
    const TSharedPtr<FJsonObject>* LevelFacets = nullptr;
    const TSharedPtr<FJsonObject>* Level = nullptr;
    TestTrue(TEXT("levelBlueprint facet reports an asset"), LevelResponse.Result->TryGetObjectField(TEXT("facets"), LevelFacets)
        && LevelFacets && (*LevelFacets)->TryGetObjectField(TEXT("levelBlueprint"), Level) && Level
        && (*Level)->GetBoolField(TEXT("exists")) && !(*Level)->GetStringField(TEXT("assetPath")).IsEmpty());

    const FOperationDefinition* LevelDefinition = FOperationRegistry::Get().Find(
        TEXT("asset.levelBlueprint.getOrCreate"));
    TestTrue(TEXT("Level Blueprint operation is directly reachable from OperationRegistry"),
        LevelDefinition && LevelDefinition->Domain == TEXT("asset"));
    FProtocolRequest Capabilities;
    Capabilities.Id = TEXT("level-capabilities");
    Capabilities.Method = TEXT("blueprint.capabilities");
    Capabilities.Params = MakeShared<FJsonObject>();
    Capabilities.Params->SetArrayField(TEXT("operationNames"), {
        MakeShared<FJsonValueString>(TEXT("asset.levelBlueprint.getOrCreate"))});
    const FProtocolResponse CapabilityResponse = FCoreService::Get().Dispatch(Capabilities);
    const TArray<TSharedPtr<FJsonValue>>* CapabilityOperations = nullptr;
    TestTrue(TEXT("Level Blueprint operation is published by OperationRegistry capabilities"),
        CapabilityResponse.IsSuccess() && CapabilityResponse.Result->TryGetArrayField(TEXT("operations"), CapabilityOperations)
        && CapabilityOperations && CapabilityOperations->Num() == 1);

    FString BlueprintFilename;
    if (!TestTrue(TEXT("Blueprint fixture saved before validate"), Fixture.Save(Blueprint, BlueprintFilename))) return false;
    Blueprint->GetOutermost()->SetDirtyFlag(false);
    TSharedRef<FJsonObject> InvalidReference = FScopedFixture::Operation(TEXT("component.add"));
    InvalidReference->SetStringField(TEXT("assetPath"), Blueprint->GetPathName());
    InvalidReference->SetStringField(TEXT("classPath"), TEXT("/Script/CodexUnrealBlueprintTests.DoesNotExist"));
    InvalidReference->SetStringField(TEXT("variableName"), TEXT("NeverAdded"));
    FProtocolRequest Validate;
    Validate.Id = TEXT("validate-preflight");
    Validate.Method = TEXT("blueprint.validate");
    Validate.Params = MakeShared<FJsonObject>();
    Validate.Params->SetArrayField(TEXT("operations"), {MakeShared<FJsonValueObject>(InvalidReference)});
    const FProtocolResponse ValidateResponse = FCoreService::Get().Dispatch(Validate);
    if (!TestTrue(TEXT("validate returns the full preflight result"), ValidateResponse.IsSuccess())) return false;
    TestFalse(TEXT("missing type reference fails validate"), ValidateResponse.Result->GetBoolField(TEXT("valid")));
    TestTrue(TEXT("validate exposes impact analysis"), ValidateResponse.Result->HasField(TEXT("impactPackages")));
    TestTrue(TEXT("validate exposes source-control inspection"), ValidateResponse.Result->HasField(TEXT("sourceControl")));
    const TArray<TSharedPtr<FJsonValue>>* Issues = nullptr;
    bool bFoundMissingReference = false;
    if (ValidateResponse.Result->TryGetArrayField(TEXT("issues"), Issues) && Issues)
    {
        for (const TSharedPtr<FJsonValue>& Value : *Issues)
        {
            const TSharedPtr<FJsonObject>* Issue = nullptr;
            FString Code;
            if (Value.IsValid() && Value->TryGetObject(Issue) && Issue
                && (*Issue)->TryGetStringField(TEXT("code"), Code) && Code == TEXT("preflight.referenceMissing"))
                bFoundMissingReference = true;
        }
    }
    TestTrue(TEXT("validate runs operation GatherPreflight type-reference checks"), bFoundMissingReference);
    TestFalse(TEXT("validate does not dirty the target package"), Blueprint->GetOutermost()->IsDirty());
    TestEqual(TEXT("validate does not apply the component operation"), CountComponents(Blueprint, TEXT("NeverAdded")), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodexUmgAndAnimE2ETest,
    "CodexUnrealBlueprint.E2E.PublicEntry.SpecializedUmgAndAnimBlueprint", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexUmgAndAnimE2ETest::RunTest(const FString& Parameters)
{
    FScopedFixture Fixture(TEXT("Specialized"));
    UWidgetBlueprint* Widget = Fixture.CreateWidgetBlueprint(TEXT("WBP_Test"));
    if (!TestNotNull(TEXT("Widget Blueprint fixture created"), Widget)) return false;
    FString WidgetFilename;
    if (!TestTrue(TEXT("clean Widget fixture saved"), Fixture.Save(Widget, WidgetFilename))) return false;
    Widget->GetOutermost()->SetDirtyFlag(false);
    const FString WidgetPath = Widget->GetPathName();

    TArray<TSharedRef<FJsonObject>> WidgetOperations;
    if (Widget->WidgetTree->RootWidget)
    {
        TSharedRef<FJsonObject> RemoveDefaultRoot = FScopedFixture::Operation(TEXT("widget.remove"));
        RemoveDefaultRoot->RemoveField(TEXT("op"));
        RemoveDefaultRoot->SetStringField(TEXT("assetPath"), WidgetPath);
        RemoveDefaultRoot->SetStringField(TEXT("widget"), Widget->WidgetTree->RootWidget->GetName());
        WidgetOperations.Add(RemoveDefaultRoot);
    }
    TSharedRef<FJsonObject> Canvas = FScopedFixture::Operation(TEXT("widget.add"));
    Canvas->RemoveField(TEXT("op")); Canvas->SetStringField(TEXT("assetPath"), WidgetPath);
    Canvas->SetStringField(TEXT("name"), TEXT("RootCanvas"));
    Canvas->SetStringField(TEXT("classPath"), UCanvasPanel::StaticClass()->GetPathName());
    WidgetOperations.Add(Canvas);
    TSharedRef<FJsonObject> Text = FScopedFixture::Operation(TEXT("widget.add"));
    Text->RemoveField(TEXT("op")); Text->SetStringField(TEXT("assetPath"), WidgetPath);
    Text->SetStringField(TEXT("name"), TEXT("StatusText"));
    Text->SetStringField(TEXT("classPath"), UTextBlock::StaticClass()->GetPathName());
    Text->SetStringField(TEXT("parent"), TEXT("RootCanvas"));
    WidgetOperations.Add(Text);
    TSharedRef<FJsonObject> Variable = FScopedFixture::Operation(TEXT("widget.variable.set"));
    Variable->RemoveField(TEXT("op")); Variable->SetStringField(TEXT("assetPath"), WidgetPath);
    Variable->SetStringField(TEXT("widget"), TEXT("StatusText"));
    Variable->SetBoolField(TEXT("isVariable"), true);
    WidgetOperations.Add(Variable);
    TSharedRef<FJsonObject> Animation = FScopedFixture::Operation(TEXT("animation.add"));
    Animation->RemoveField(TEXT("op")); Animation->SetStringField(TEXT("assetPath"), WidgetPath);
    Animation->SetStringField(TEXT("name"), TEXT("Pulse"));
    WidgetOperations.Add(Animation);
    FJobSnapshot WidgetSnapshot;
    FString Error;
    if (!TestTrue(TEXT("UMG operations pass through Core Dispatch and OperationRegistry"),
        DispatchOperations(Fixture.GetRunId() + TEXT("_umg"), WidgetOperations, WidgetSnapshot, Error)))
    {
        AddError(Error);
        return false;
    }
    Widget = nullptr;
    UObject* ReloadedWidget = nullptr;
    if (!TestTrue(TEXT("Widget fixture unloads and reloads from disk"),
        Fixture.UnloadAndReload(Fixture.Package(TEXT("WBP_Test")), ReloadedWidget))) return false;
    Widget = Cast<UWidgetBlueprint>(ReloadedWidget);
    UWidget* RootCanvas = Widget && Widget->WidgetTree ? Widget->WidgetTree->FindWidget(TEXT("RootCanvas")) : nullptr;
    UWidget* StatusText = Widget && Widget->WidgetTree ? Widget->WidgetTree->FindWidget(TEXT("StatusText")) : nullptr;
    TestNotNull(TEXT("independent expected root survives save and reload"), RootCanvas);
    TestNotNull(TEXT("independent expected child survives save and reload"), StatusText);
    if (StatusText)
    {
        TestTrue(TEXT("independent expected variable flag survives reload"), StatusText->bIsVariable);
        TestEqual(TEXT("independent expected parent survives reload"), StatusText->GetParent(), Cast<UPanelWidget>(RootCanvas));
    }
    TestTrue(TEXT("independent expected animation survives reload"), Widget && Widget->Animations.ContainsByPredicate(
        [](const UWidgetAnimation* Item) { return Item && Item->GetName() == TEXT("Pulse"); }));

    UAnimBlueprint* Anim = Fixture.CreateAnimBlueprint(TEXT("ABP_Test"));
    if (!Anim)
    {
        AddError(TEXT("No USkeleton is available; AnimBP E2E requires a real UE skeleton asset."));
        return false;
    }
    FString AnimFilename;
    if (!TestTrue(TEXT("clean AnimBP fixture saved"), Fixture.Save(Anim, AnimFilename))) return false;
    Anim->GetOutermost()->SetDirtyFlag(false);
    const FString AnimPath = Anim->GetPathName();
    TSharedRef<FJsonObject> AddVariable = FScopedFixture::Operation(TEXT("anim.variable.add"));
    AddVariable->RemoveField(TEXT("op")); AddVariable->SetStringField(TEXT("assetPath"), AnimPath);
    AddVariable->SetStringField(TEXT("name"), TEXT("Speed"));
    TSharedRef<FJsonObject> FloatType = MakeShared<FJsonObject>();
    FloatType->SetStringField(TEXT("category"), UEdGraphSchema_K2::PC_Float.ToString());
    AddVariable->SetObjectField(TEXT("type"), FloatType);
    FJobSnapshot AnimSnapshot;
    if (!TestTrue(TEXT("AnimBP operation passes through Core Dispatch and OperationRegistry"),
        DispatchOperations(Fixture.GetRunId() + TEXT("_anim"), { AddVariable }, AnimSnapshot, Error)))
    {
        AddError(Error);
        return false;
    }
    Anim = nullptr;
    UObject* ReloadedAnim = nullptr;
    if (!TestTrue(TEXT("AnimBP fixture unloads and reloads from disk"),
        Fixture.UnloadAndReload(Fixture.Package(TEXT("ABP_Test")), ReloadedAnim))) return false;
    Anim = Cast<UAnimBlueprint>(ReloadedAnim);
    TestNotNull(TEXT("AnimBP survives save, unload, and reload"), Anim);
    if (Anim) TestTrue(TEXT("independent expected AnimBP variable survives reload"),
        FBlueprintEditorUtils::FindNewVariableIndex(Anim, TEXT("Speed")) != INDEX_NONE);
    return true;
}

#endif
