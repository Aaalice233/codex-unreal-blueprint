#include "CodexUnrealBlueprintInspection.h"

#include "Animation/AnimBlueprint.h"
#include "WidgetBlueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Engine/World.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/SecureHash.h"
#include "CodexUnrealBlueprintAnimOperations.h"
#include "CodexUnrealBlueprintAssetOperations.h"
#include "CodexUnrealBlueprintComponentOperations.h"
#include "CodexUnrealBlueprintTypeSystem.h"
#include "CodexUnrealBlueprintUmgOperations.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace CodexUnrealBlueprint
{
    namespace
    {
        TSharedRef<FJsonObject> ErrorResult() { return MakeShared<FJsonObject>(); }

        TSharedRef<FJsonObject> GraphSnapshot(UBlueprint* Blueprint)
        {
            TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
            TArray<UEdGraph*> Graphs; Blueprint->GetAllGraphs(Graphs);
            Graphs.RemoveAll([](const UEdGraph* Graph) { return Graph == nullptr; });
            Graphs.Sort([](const UEdGraph& A, const UEdGraph& B) { return A.GraphGuid < B.GraphGuid; });
            TArray<TSharedPtr<FJsonValue>> Values;
            for (const UEdGraph* Graph : Graphs)
            {
                TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
                Item->SetStringField(TEXT("name"), Graph->GetName());
                Item->SetStringField(TEXT("path"), Graph->GetPathName());
                Item->SetStringField(TEXT("guid"), Graph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphens).ToLower());
                Item->SetStringField(TEXT("schema"), Graph->GetSchema() ? Graph->GetSchema()->GetClass()->GetPathName() : FString());
                TArray<TSharedPtr<FJsonValue>> Nodes;
                for (const UEdGraphNode* Node : Graph->Nodes)
                {
                    if (!Node) continue;
                    TSharedRef<FJsonObject> NodeJson = MakeShared<FJsonObject>();
                    NodeJson->SetStringField(TEXT("guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens).ToLower());
                    NodeJson->SetStringField(TEXT("classPath"), Node->GetClass()->GetPathName());
                    NodeJson->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
                    NodeJson->SetNumberField(TEXT("x"), Node->NodePosX); NodeJson->SetNumberField(TEXT("y"), Node->NodePosY);
                    TArray<TSharedPtr<FJsonValue>> Pins;
                    for (const UEdGraphPin* Pin : Node->Pins)
                    {
                        if (!Pin) continue;
                        TSharedRef<FJsonObject> PinJson = MakeShared<FJsonObject>();
                        PinJson->SetStringField(TEXT("guid"), Pin->PinId.ToString(EGuidFormats::DigitsWithHyphens).ToLower());
                        PinJson->SetStringField(TEXT("name"), Pin->PinName.ToString());
                        PinJson->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
                        PinJson->SetObjectField(TEXT("type"), FBlueprintTypeSystem::PinTypeToJson(Pin->PinType));
                        PinJson->SetStringField(TEXT("defaultValue"), Pin->DefaultValue);
                        Pins.Add(MakeShared<FJsonValueObject>(PinJson));
                    }
                    NodeJson->SetArrayField(TEXT("pins"), Pins); Nodes.Add(MakeShared<FJsonValueObject>(NodeJson));
                }
                Item->SetArrayField(TEXT("nodes"), Nodes); Values.Add(MakeShared<FJsonValueObject>(Item));
            }
            Result->SetArrayField(TEXT("graphs"), Values); return Result;
        }

        TSharedRef<FJsonObject> VariableSnapshot(UBlueprint* Blueprint)
        {
            TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>(); TArray<TSharedPtr<FJsonValue>> Values;
            for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
            {
                TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("name"), Variable.VarName.ToString()); Item->SetStringField(TEXT("guid"), Variable.VarGuid.ToString(EGuidFormats::DigitsWithHyphens).ToLower()); Item->SetObjectField(TEXT("type"), FBlueprintTypeSystem::PinTypeToJson(Variable.VarType)); Item->SetStringField(TEXT("category"), Variable.Category.ToString()); Values.Add(MakeShared<FJsonValueObject>(Item));
            }
            Result->SetArrayField(TEXT("variables"), Values); return Result;
        }

        TSharedRef<FJsonObject> PaginateFacet(const TSharedRef<FJsonObject>& Full, const int32 Offset,
            const int32 Limit, bool& bOutHasMore)
        {
            TSharedRef<FJsonObject> Page = MakeShared<FJsonObject>();
            bOutHasMore = false;
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Full->Values)
            {
                if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Array)
                {
                    Page->SetField(Pair.Key, Pair.Value);
                    continue;
                }
                const TArray<TSharedPtr<FJsonValue>>& Values = Pair.Value->AsArray();
                TArray<TSharedPtr<FJsonValue>> PageValues;
                const int32 End = FMath::Min(Values.Num(), Offset + Limit);
                for (int32 Index = FMath::Min(Offset, Values.Num()); Index < End; ++Index) PageValues.Add(Values[Index]);
                Page->SetArrayField(Pair.Key, PageValues);
                Page->SetNumberField(Pair.Key + TEXT("Total"), Values.Num());
                bOutHasMore = bOutHasMore || End < Values.Num();
            }
            return Page;
        }
    }

    bool FBlueprintInspection::Inspect(const FString& AssetPath, const TArray<FString>& Facets,
        const int32 Offset, const int32 Limit, TSharedRef<FJsonObject>& OutResult, FProtocolError& OutError)
    {
        return Inspect(AssetPath, Facets, {}, Offset, Limit, OutResult, OutError);
    }

    bool FBlueprintInspection::Inspect(const FString& AssetPath, const TArray<FString>& Facets,
        const TArray<FString>& ClassDefaultPropertyPaths, const int32 Offset, const int32 Limit,
        TSharedRef<FJsonObject>& OutResult, FProtocolError& OutError)
    {
        if (!IsInGameThread()) { OutError = FProtocolError::Make(EErrorCode::InternalError, TEXT("Blueprint inspection must run on the game thread."), TEXT("FBlueprintInspection::Inspect")); return false; }
        UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath, nullptr, LOAD_NoWarn);
        if (!Asset) { OutError = FProtocolError::Make(EErrorCode::AssetNotFound, FString::Printf(TEXT("Asset '%s' was not found."), *AssetPath), TEXT("StaticLoadObject")); OutError.AssetPath = AssetPath; return false; }
        UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
        UWorld* World = Cast<UWorld>(Asset);
        if (!Blueprint && !World) { OutError = FProtocolError::Make(EErrorCode::InvalidArgument, TEXT("blueprint.inspect requires a Blueprint-family asset or Map."), TEXT("Cast<UBlueprint/UWorld>")); OutError.AssetPath = AssetPath; return false; }

        TArray<FString> Requested = Facets;
        if (!Requested.Num()) Requested = World
            ? TArray<FString>{TEXT("levelBlueprint")}
            : TArray<FString>{TEXT("asset"), TEXT("compile"), TEXT("variables"), TEXT("components"), TEXT("graphs")};
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>(); Result->SetStringField(TEXT("assetPath"), Asset->GetPathName());
        Result->SetStringField(TEXT("packageName"), Asset->GetOutermost()->GetName());
        TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
        for (const FString& Facet : Requested)
        {
            if (Facet == TEXT("levelBlueprint"))
            {
                if (!World || !World->PersistentLevel) { OutError = FProtocolError::Make(EErrorCode::InvalidArgument, TEXT("The levelBlueprint facet requires a loaded Map with a persistent level."), TEXT("FBlueprintInspection::Inspect")); OutError.AssetPath = AssetPath; return false; }
                ULevelScriptBlueprint* LevelBlueprint = World->PersistentLevel->GetLevelScriptBlueprint(true);
                TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
                Json->SetBoolField(TEXT("exists"), LevelBlueprint != nullptr);
                Json->SetStringField(TEXT("assetPath"), LevelBlueprint ? LevelBlueprint->GetPathName() : FString());
                Json->SetStringField(TEXT("owningMapPath"), World->GetPathName());
                Data->SetObjectField(Facet, Json);
                continue;
            }
            if (!Blueprint) { OutError = FProtocolError::Make(EErrorCode::InvalidArgument, FString::Printf(TEXT("Facet '%s' requires a Blueprint-family asset."), *Facet), TEXT("FBlueprintInspection::Inspect")); OutError.AssetPath = AssetPath; return false; }
            if (Facet == TEXT("asset")) { TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>(); Json->SetStringField(TEXT("classPath"), Blueprint->GetClass()->GetPathName()); Json->SetStringField(TEXT("parentClassPath"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : FString()); Json->SetStringField(TEXT("generatedClassPath"), Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : FString()); Data->SetObjectField(Facet, Json); }
            else if (Facet == TEXT("compile")) { TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>(); Json->SetBoolField(TEXT("upToDate"), Blueprint->IsUpToDate()); Json->SetBoolField(TEXT("packageDirty"), Blueprint->GetOutermost()->IsDirty()); Data->SetObjectField(Facet, Json); }
            else if (Facet == TEXT("variables")) Data->SetObjectField(Facet, VariableSnapshot(Blueprint));
            else if (Facet == TEXT("classDefaults"))
            {
                TArray<FString> PropertyPaths = ClassDefaultPropertyPaths;
                if (PropertyPaths.Num() == 0)
                {
                    for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
                        PropertyPaths.Add(Variable.VarName.ToString());
                }
                const FBlueprintOperationResult Defaults = FBlueprintAssetOperations::ReadClassDefaults(Blueprint, PropertyPaths);
                if (!Defaults.bSuccess)
                {
                    const FString Message = Defaults.Error.IsSet()
                        ? Defaults.Error.GetValue().Message : TEXT("Class default inspection failed.");
                    const FString Callsite = Defaults.Error.IsSet()
                        ? Defaults.Error.GetValue().UECallsite : TEXT("FBlueprintAssetOperations::ReadClassDefaults");
                    OutError = FProtocolError::Make(EErrorCode::ValidationFailed, Message, Callsite);
                    OutError.AssetPath = Defaults.Error.IsSet() ? Defaults.Error.GetValue().AssetPath : AssetPath;
                    return false;
                }
                Data->SetObjectField(Facet, Defaults.Data);
            }
            else if (Facet == TEXT("components")) { FBlueprintOperationResult Components = FBlueprintComponentOperations::List(Blueprint, true); if (!Components.bSuccess) { OutError = FProtocolError::Make(EErrorCode::ValidationFailed, Components.Error.IsSet() ? Components.Error.GetValue().Message : TEXT("Component inspection failed."), TEXT("FBlueprintComponentOperations::List")); OutError.AssetPath = AssetPath; return false; } Data->SetObjectField(Facet, Components.Data); }
            else if (Facet == TEXT("graphs") || Facet == TEXT("nodes") || Facet == TEXT("pins")) Data->SetObjectField(Facet, GraphSnapshot(Blueprint));
            else if (Facet == TEXT("umg")) { UWidgetBlueprint* Widget = Cast<UWidgetBlueprint>(Blueprint); TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>(); FUmgOperationError Error; if (!Widget || !FBlueprintUmgOperations::Inspect(Widget, Snapshot, Error)) { OutError = FProtocolError::Make(EErrorCode::ValidationFailed, Error.Message.IsEmpty() ? TEXT("The asset is not a Widget Blueprint.") : Error.Message, Error.UECallsite); OutError.AssetPath = AssetPath; return false; } Data->SetObjectField(Facet, Snapshot); }
            else if (Facet == TEXT("anim")) { UAnimBlueprint* Anim = Cast<UAnimBlueprint>(Blueprint); TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>(); FAnimOperationError Error; if (!Anim || !FBlueprintAnimOperations::Inspect(Anim, Snapshot, Error)) { OutError = FProtocolError::Make(EErrorCode::ValidationFailed, Error.Message.IsEmpty() ? TEXT("The asset is not an Animation Blueprint.") : Error.Message, Error.UECallsite); OutError.AssetPath = AssetPath; return false; } Data->SetObjectField(Facet, Snapshot); }
            else { OutError = FProtocolError::Make(EErrorCode::InvalidArgument, FString::Printf(TEXT("Unknown inspection facet '%s'."), *Facet), TEXT("FBlueprintInspection::Inspect")); OutError.AssetPath = AssetPath; return false; }
        }
        FString Canonical; TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Canonical); FJsonSerializer::Serialize(Data, Writer); FTCHARToUTF8 Utf8(*Canonical); uint8 Digest[FSHA1::DigestSize]; FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Digest);
        TSharedRef<FJsonObject> PagedData = MakeShared<FJsonObject>();
        bool bHasMore = false;
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Data->Values)
        {
            if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Object)
            {
                bool bFacetHasMore = false;
                PagedData->SetObjectField(Pair.Key, PaginateFacet(Pair.Value->AsObject().ToSharedRef(), Offset, Limit, bFacetHasMore));
                bHasMore = bHasMore || bFacetHasMore;
            }
            else PagedData->SetField(Pair.Key, Pair.Value);
        }
        Result->SetObjectField(TEXT("facets"), PagedData);
        Result->SetStringField(TEXT("structureHash"), BytesToHex(Digest, FSHA1::DigestSize).ToLower());
        Result->SetNumberField(TEXT("offset"), Offset);
        Result->SetNumberField(TEXT("limit"), Limit);
        if (bHasMore) Result->SetStringField(TEXT("nextCursor"), FString::FromInt(Offset + Limit));
        OutResult = Result; return true;
    }
}
