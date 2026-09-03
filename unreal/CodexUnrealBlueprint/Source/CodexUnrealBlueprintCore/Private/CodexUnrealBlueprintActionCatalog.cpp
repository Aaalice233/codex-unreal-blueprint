#include "CodexUnrealBlueprintActionCatalog.h"

#include "BlueprintActionDatabase.h"
#include "BlueprintActionFilter.h"
#include "BlueprintNodeSpawner.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "HAL/PlatformTLS.h"
#include "Misc/SecureHash.h"

namespace CodexUnrealBlueprint
{
    namespace
    {
        struct FCatalogRecord
        {
            UObject* Owner = nullptr;
            UBlueprintNodeSpawner* Spawner = nullptr;
            FString Canonical;
            FString Digest;
        };

        FString Sha1(const FString& Value)
        {
            const FTCHARToUTF8 Utf8(*Value);
            uint8 Hash[FSHA1::DigestSize];
            FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Hash);
            return BytesToHex(Hash, FSHA1::DigestSize).ToLower();
        }

        FString ObjectPath(const UObject* Object)
        {
            return Object ? Object->GetPathName() : TEXT("<none>");
        }

        FString Canonicalize(UObject* Owner, UBlueprintNodeSpawner* Spawner)
        {
            const FString Signature = Spawner->GetSpawnerSignature().ToString();
            return FString::Printf(TEXT("owner=%s\nspawner=%s\nnode=%s\nsignature=%s"),
                *ObjectPath(Owner), *Spawner->GetClass()->GetPathName(),
                Spawner->NodeClass ? *Spawner->NodeClass->GetPathName() : TEXT("<none>"), *Signature);
        }

        bool BuildCatalog(TArray<FCatalogRecord>& OutRecords, FString& OutFingerprint, FGraphActionError& OutError)
        {
            OutRecords.Reset();
            OutFingerprint.Reset();
            if (!IsInGameThread())
            {
                OutError.Code = TEXT("GraphActionWrongThread");
                OutError.Message = TEXT("The Blueprint Action Catalog must be queried on the game thread.");
                OutError.UECallsite = TEXT("FBlueprintGraphActionCatalog::BuildCatalog");
                return false;
            }

            const FBlueprintActionDatabase::FActionRegistry& Registry = FBlueprintActionDatabase::Get().GetAllActions();
            for (const TPair<FObjectKey, FBlueprintActionDatabase::FActionList>& Pair : Registry)
            {
                UObject* Owner = Pair.Key.ResolveObjectPtr();
                for (UBlueprintNodeSpawner* Spawner : Pair.Value)
                {
                    if (!IsValid(Spawner) || !Spawner->NodeClass)
                    {
                        continue;
                    }
                    FCatalogRecord& Record = OutRecords.AddDefaulted_GetRef();
                    Record.Owner = Owner;
                    Record.Spawner = Spawner;
                    Record.Canonical = Canonicalize(Owner, Spawner);
                    Record.Digest = Sha1(Record.Canonical);
                }
            }
            OutRecords.Sort([](const FCatalogRecord& A, const FCatalogRecord& B)
            {
                if (A.Canonical != B.Canonical) return A.Canonical < B.Canonical;
                return A.Spawner->GetPathName() < B.Spawner->GetPathName();
            });
            FString Manifest;
            for (const FCatalogRecord& Record : OutRecords)
            {
                Manifest += Record.Canonical;
                Manifest += TEXT("\n--\n");
            }
            OutFingerprint = Sha1(Manifest);
            return true;
        }

        FBlueprintActionContext MakeContext(UBlueprint* Blueprint, UEdGraph* Graph)
        {
            FBlueprintActionContext Context;
            if (Blueprint) Context.Blueprints.Add(Blueprint);
            if (Graph) Context.Graphs.Add(Graph);
            return Context;
        }

        bool IsCompatible(const FCatalogRecord& Record, UBlueprint* Blueprint, UEdGraph* Graph)
        {
            if (!Blueprint || !Graph) return false;
            FBlueprintActionFilter Filter;
            Filter.Context = MakeContext(Blueprint, Graph);
            FBlueprintActionInfo Info(Record.Owner, Record.Spawner);
            return !Filter.IsFiltered(Info);
        }

        TSharedRef<FJsonObject> PinToJson(const UEdGraphPin& Pin)
        {
            TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
            Json->SetStringField(TEXT("pinId"), Pin.PinId.ToString(EGuidFormats::DigitsWithHyphens));
            Json->SetStringField(TEXT("name"), Pin.PinName.ToString());
            Json->SetStringField(TEXT("direction"), Pin.Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
            Json->SetStringField(TEXT("category"), Pin.PinType.PinCategory.ToString());
            Json->SetStringField(TEXT("subCategory"), Pin.PinType.PinSubCategory.ToString());
            Json->SetStringField(TEXT("subCategoryObject"), ObjectPath(Pin.PinType.PinSubCategoryObject.Get()));
            Json->SetStringField(TEXT("container"), Pin.PinType.ContainerType == EPinContainerType::Array ? TEXT("array")
                : Pin.PinType.ContainerType == EPinContainerType::Set ? TEXT("set")
                : Pin.PinType.ContainerType == EPinContainerType::Map ? TEXT("map") : TEXT("none"));
            Json->SetBoolField(TEXT("reference"), Pin.PinType.bIsReference);
            Json->SetBoolField(TEXT("const"), Pin.PinType.bIsConst);
            return Json;
        }

        TSharedRef<FJsonObject> RecordToJson(const FCatalogRecord& Record, const FString& Fingerprint,
            UBlueprint* Blueprint, UEdGraph* Graph, const bool bCompatible)
        {
            FBlueprintActionContext Context = MakeContext(Blueprint, Graph);
            const FBlueprintActionUiSpec Ui = Record.Spawner->GetUiSpec(Context, IBlueprintNodeBinder::FBindingSet());
            FBlueprintActionInfo Info(Record.Owner, Record.Spawner);
            TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
            Json->SetStringField(TEXT("actionId"), FString::Printf(TEXT("bpact:%s:%s"), *Fingerprint, *Record.Digest));
            Json->SetStringField(TEXT("catalogFingerprint"), Fingerprint);
            Json->SetStringField(TEXT("name"), Ui.MenuName.ToString());
            Json->SetStringField(TEXT("category"), Ui.Category.ToString());
            Json->SetStringField(TEXT("tooltip"), Ui.Tooltip.ToString());
            Json->SetStringField(TEXT("keywords"), Ui.Keywords.ToString());
            Json->SetStringField(TEXT("ownerPath"), ObjectPath(Record.Owner));
            Json->SetStringField(TEXT("spawnerClass"), Record.Spawner->GetClass()->GetPathName());
            Json->SetStringField(TEXT("nodeClass"), Record.Spawner->NodeClass->GetPathName());
            Json->SetBoolField(TEXT("contextCompatible"), bCompatible);

            if (const UFunction* Function = Info.GetAssociatedFunction())
            {
                Json->SetStringField(TEXT("memberSignature"), Function->GetPathName());
            }
            else if (const FProperty* Property = Info.GetAssociatedProperty())
            {
                Json->SetStringField(TEXT("memberSignature"), Property->GetPathName());
            }
            else
            {
                Json->SetStringField(TEXT("memberSignature"), Record.Spawner->GetSpawnerSignature().ToString());
            }

            TArray<TSharedPtr<FJsonValue>> Pins;
            if (UEdGraphNode* Template = Record.Spawner->GetTemplateNode(Graph))
            {
                for (const UEdGraphPin* Pin : Template->Pins)
                {
                    if (Pin) Pins.Add(MakeShared<FJsonValueObject>(PinToJson(*Pin)));
                }
            }
            Json->SetArrayField(TEXT("pins"), Pins);
            return Json;
        }

        bool IsSha1Text(const FString& Value)
        {
            if (Value.Len() != 40) return false;
            for (const TCHAR Character : Value)
            {
                if (!FChar::IsHexDigit(Character)) return false;
            }
            return true;
        }

        bool ParseActionId(const FString& ActionId, FString& OutFingerprint, FString& OutDigest)
        {
            TArray<FString> Parts;
            ActionId.ParseIntoArray(Parts, TEXT(":"), false);
            if (Parts.Num() != 3 || Parts[0] != TEXT("bpact") || !IsSha1Text(Parts[1]) || !IsSha1Text(Parts[2]))
                return false;
            OutFingerprint = Parts[1].ToLower();
            OutDigest = Parts[2].ToLower();
            return true;
        }
    }

    TSharedRef<FJsonObject> FGraphActionSearchResult::ToJson() const
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("catalogFingerprint"), CatalogFingerprint);
        Json->SetNumberField(TEXT("totalMatches"), TotalMatches);
        Json->SetArrayField(TEXT("actions"), Actions);
        if (!NextCursor.IsEmpty()) Json->SetStringField(TEXT("nextCursor"), NextCursor);
        return Json;
    }

    FString FBlueprintGraphActionCatalog::GetFingerprint(FGraphActionError& OutError)
    {
        OutError = FGraphActionError();
        TArray<FCatalogRecord> Records;
        FString Fingerprint;
        BuildCatalog(Records, Fingerprint, OutError);
        return Fingerprint;
    }

    bool FBlueprintGraphActionCatalog::Search(const FGraphActionSearchRequest& Request,
        FGraphActionSearchResult& OutResult, FGraphActionError& OutError)
    {
        OutResult = FGraphActionSearchResult();
        OutError = FGraphActionError();
        OutError.OperationIndex = Request.OperationIndex;
        if (!IsInGameThread())
        {
            OutError.Code = TEXT("GraphActionWrongThread");
            OutError.Message = TEXT("The Blueprint Action Catalog must be queried on the game thread.");
            OutError.UECallsite = TEXT("FBlueprintGraphActionCatalog::Search");
            return false;
        }
        if (!Request.Blueprint || !Request.Graph)
        {
            OutError.Code = TEXT("GraphActionContextRequired");
            OutError.Message = TEXT("Action search requires an exact Blueprint and Graph context.");
            OutError.UECallsite = TEXT("FBlueprintGraphActionCatalog::Search");
            return false;
        }
        if (Request.PageSize < 1 || Request.PageSize > 200)
        {
            OutError.Code = TEXT("GraphActionInvalidPageSize");
            OutError.Message = TEXT("pageSize must be between 1 and 200.");
            OutError.UECallsite = TEXT("FBlueprintGraphActionCatalog::Search");
            return false;
        }

        TArray<FCatalogRecord> Records;
        if (!BuildCatalog(Records, OutResult.CatalogFingerprint, OutError)) return false;
        int32 Offset = 0;
        if (!Request.Cursor.IsEmpty())
        {
            FString CursorFingerprint;
            FString CursorOffset;
            if (!Request.Cursor.Split(TEXT(":"), &CursorFingerprint, &CursorOffset, ESearchCase::CaseSensitive, ESearchDir::FromEnd)
                || CursorFingerprint != OutResult.CatalogFingerprint || !LexTryParseString(Offset, *CursorOffset) || Offset < 0)
            {
                OutError.Code = TEXT("GraphActionStaleCursor");
                OutError.Message = TEXT("The search cursor is malformed or belongs to a different catalog fingerprint.");
                OutError.UECallsite = TEXT("FBlueprintGraphActionCatalog::Search");
                return false;
            }
        }

        const FString Needle = Request.Query.TrimStartAndEnd().ToLower();
        TArray<TPair<const FCatalogRecord*, bool>> Matches;
        for (const FCatalogRecord& Record : Records)
        {
            const bool bCompatible = IsCompatible(Record, Request.Blueprint, Request.Graph);
            if (Request.bCompatibleOnly && !bCompatible) continue;
            const FBlueprintActionUiSpec& Ui = Record.Spawner->PrimeDefaultUiSpec(Request.Graph);
            const FString Haystack = (Ui.MenuName.ToString() + TEXT(" ") + Ui.Category.ToString() + TEXT(" ")
                + Ui.Keywords.ToString() + TEXT(" ") + Record.Canonical).ToLower();
            if (Needle.IsEmpty() || Haystack.Contains(Needle)) Matches.Emplace(&Record, bCompatible);
        }
        OutResult.TotalMatches = Matches.Num();
        if (Offset > Matches.Num())
        {
            OutError.Code = TEXT("GraphActionCursorOutOfRange");
            OutError.Message = TEXT("The search cursor offset is beyond the current result set.");
            OutError.UECallsite = TEXT("FBlueprintGraphActionCatalog::Search");
            return false;
        }
        const int32 End = FMath::Min(Offset + Request.PageSize, Matches.Num());
        for (int32 Index = Offset; Index < End; ++Index)
        {
            OutResult.Actions.Add(MakeShared<FJsonValueObject>(RecordToJson(*Matches[Index].Key,
                OutResult.CatalogFingerprint, Request.Blueprint, Request.Graph, Matches[Index].Value)));
        }
        if (End < Matches.Num()) OutResult.NextCursor = FString::Printf(TEXT("%s:%d"), *OutResult.CatalogFingerprint, End);
        return true;
    }

    bool FBlueprintGraphActionCatalog::Resolve(const FString& ActionId, UBlueprint* Blueprint, UEdGraph* Graph,
        UBlueprintNodeSpawner*& OutSpawner, FGraphActionError& OutError, const int32 OperationIndex)
    {
        OutSpawner = nullptr;
        OutError = FGraphActionError();
        OutError.OperationIndex = OperationIndex;
        if (!IsInGameThread())
        {
            OutError.Code = TEXT("GraphActionWrongThread");
            OutError.Message = TEXT("Blueprint actions must be resolved on the game thread.");
            OutError.UECallsite = TEXT("FBlueprintGraphActionCatalog::Resolve");
            return false;
        }
        if (!Blueprint || !Graph)
        {
            OutError.Code = TEXT("GraphActionContextRequired");
            OutError.Message = TEXT("Action resolution requires an exact Blueprint and Graph context.");
            OutError.UECallsite = TEXT("FBlueprintGraphActionCatalog::Resolve");
            OutError.AssetPath = ObjectPath(Blueprint);
            OutError.GraphPath = ObjectPath(Graph);
            return false;
        }
        FString RequestedFingerprint;
        FString RequestedDigest;
        if (!ParseActionId(ActionId, RequestedFingerprint, RequestedDigest))
        {
            OutError.Code = TEXT("GraphActionInvalidId");
            OutError.Message = TEXT("actionId must use bpact:<40-hex fingerprint>:<40-hex action>.");
            OutError.UECallsite = TEXT("FBlueprintGraphActionCatalog::Resolve");
            return false;
        }
        TArray<FCatalogRecord> Records;
        FString CurrentFingerprint;
        if (!BuildCatalog(Records, CurrentFingerprint, OutError)) return false;
        if (RequestedFingerprint != CurrentFingerprint)
        {
            OutError.Code = TEXT("GraphActionCatalogChanged");
            OutError.Message = FString::Printf(TEXT("actionId catalog fingerprint '%s' does not match current '%s'."),
                *RequestedFingerprint, *CurrentFingerprint);
            OutError.UECallsite = TEXT("FBlueprintGraphActionCatalog::Resolve");
            return false;
        }
        TArray<const FCatalogRecord*> Candidates;
        for (const FCatalogRecord& Record : Records)
        {
            if (Record.Digest == RequestedDigest) Candidates.Add(&Record);
        }
        if (Candidates.Num() == 0)
        {
            OutError.Code = TEXT("GraphActionNotFound");
            OutError.Message = TEXT("The action no longer exists in the current catalog.");
            OutError.UECallsite = TEXT("FBlueprintGraphActionCatalog::Resolve");
            return false;
        }
        if (Candidates.Num() != 1)
        {
            OutError.Code = TEXT("GraphActionAmbiguous");
            OutError.Message = TEXT("The actionId resolves to multiple registered spawners; no candidate was selected.");
            OutError.UECallsite = TEXT("FBlueprintGraphActionCatalog::Resolve");
            for (const FCatalogRecord* Candidate : Candidates) OutError.Candidates.Add(Candidate->Canonical);
            return false;
        }
        if (!IsCompatible(*Candidates[0], Blueprint, Graph))
        {
            OutError.Code = TEXT("GraphActionIncompatible");
            OutError.Message = TEXT("The action is not compatible with the exact Blueprint/Graph context.");
            OutError.UECallsite = TEXT("FBlueprintGraphActionCatalog::Resolve");
            OutError.AssetPath = ObjectPath(Blueprint);
            OutError.GraphPath = ObjectPath(Graph);
            return false;
        }
        OutSpawner = Candidates[0]->Spawner;
        return true;
    }

    bool FBlueprintGraphActionCatalog::Spawn(const FString& ActionId, UBlueprint* Blueprint, UEdGraph* Graph,
        const FVector2D& Location, UEdGraphNode*& OutNode, FGraphActionError& OutError, const int32 OperationIndex)
    {
        OutNode = nullptr;
        UBlueprintNodeSpawner* Spawner = nullptr;
        if (!Resolve(ActionId, Blueprint, Graph, Spawner, OutError, OperationIndex)) return false;
        Graph->Modify();
        OutNode = Spawner->Invoke(Graph, IBlueprintNodeBinder::FBindingSet(), Location);
        if (!OutNode || OutNode->GetGraph() != Graph)
        {
            OutError.Code = TEXT("GraphActionSpawnFailed");
            OutError.Message = TEXT("The resolved BlueprintNodeSpawner did not create a node in the requested graph.");
            OutError.UECallsite = TEXT("UBlueprintNodeSpawner::Invoke");
            OutError.AssetPath = ObjectPath(Blueprint);
            OutError.GraphPath = ObjectPath(Graph);
            OutNode = nullptr;
            return false;
        }
        OutNode->Modify();
        if (!OutNode->NodeGuid.IsValid()) OutNode->CreateNewGuid();
        return true;
    }
}
