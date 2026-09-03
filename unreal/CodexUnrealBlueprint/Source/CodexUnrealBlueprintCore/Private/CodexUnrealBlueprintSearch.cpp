#include "CodexUnrealBlueprintSearch.h"

#include "AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Modules/ModuleManager.h"
#include "CodexUnrealBlueprintActionCatalog.h"
#include "CodexUnrealBlueprintOperationRegistry.h"
#include "UObject/UObjectIterator.h"

namespace CodexUnrealBlueprint
{
    namespace
    {
        TSharedRef<FJsonObject> Paged(const TArray<TSharedPtr<FJsonValue>>& All, const int32 Offset, const int32 Limit)
        {
            TArray<TSharedPtr<FJsonValue>> Page; const int32 End = FMath::Min(All.Num(), Offset + Limit);
            for (int32 Index = Offset; Index < End; ++Index) Page.Add(All[Index]);
            TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>(); Result->SetNumberField(TEXT("total"), All.Num()); Result->SetArrayField(TEXT("items"), Page); if (End < All.Num()) Result->SetStringField(TEXT("nextCursor"), FString::FromInt(End)); return Result;
        }

        bool Match(const FString& Value, const FString& Query) { return Value.Contains(Query, ESearchCase::IgnoreCase); }

        bool ParseCursor(const FString& Cursor, int32& OutOffset)
        {
            OutOffset = 0;
            if (Cursor.IsEmpty()) return true;
            for (const TCHAR Character : Cursor) if (!FChar::IsDigit(Character)) return false;
            const int64 Value = FCString::Atoi64(*Cursor);
            if (Value < 0 || Value > MAX_int32) return false;
            OutOffset = static_cast<int32>(Value);
            return true;
        }
    }

    bool FBlueprintSearch::Search(const FString& Query, const FString& Domain,
        const TSharedPtr<FJsonObject>& Context, const FString& Cursor, const int32 Limit,
        TSharedRef<FJsonObject>& OutResult, FProtocolError& OutError)
    {
        if (!IsInGameThread()) { OutError = FProtocolError::Make(EErrorCode::InternalError, TEXT("Blueprint search must run on the game thread."), TEXT("FBlueprintSearch::Search")); return false; }
        int32 Offset = 0;
        if (!ParseCursor(Cursor, Offset) || Limit < 1 || Limit > 200)
        {
            OutError = FProtocolError::Make(EErrorCode::InvalidArgument,
                TEXT("cursor must be a non-negative decimal offset and limit must be between 1 and 200."),
                TEXT("FBlueprintSearch::Search"));
            return false;
        }
        if (Domain == TEXT("operation")) { OutResult = FOperationRegistry::Get().Search(Query, Domain, Offset, Limit); return true; }
        if (Domain == TEXT("action"))
        {
            FGraphActionSearchRequest Request; Request.Query = Query; Request.PageSize = Limit; Request.Cursor = Cursor;
            FString AssetPath, GraphGuidText, GraphName;
            if (Context.IsValid()) { Context->TryGetStringField(TEXT("assetPath"), AssetPath); Context->TryGetStringField(TEXT("graphGuid"), GraphGuidText); Context->TryGetStringField(TEXT("graphName"), GraphName); }
            if (!AssetPath.IsEmpty()) Request.Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath, nullptr, LOAD_NoWarn);
            if (!Request.Blueprint) { OutError = FProtocolError::Make(EErrorCode::InvalidArgument, TEXT("Action search requires context.assetPath resolving to a Blueprint."), TEXT("FBlueprintSearch::Search")); OutError.AssetPath = AssetPath; return false; }
            TArray<UEdGraph*> Graphs; Request.Blueprint->GetAllGraphs(Graphs); FGuid GraphGuid; FGuid::Parse(GraphGuidText, GraphGuid);
            for (UEdGraph* Graph : Graphs) if (Graph && ((!GraphGuidText.IsEmpty() && Graph->GraphGuid == GraphGuid) || (!GraphName.IsEmpty() && Graph->GetName() == GraphName))) { Request.Graph = Graph; break; }
            if (!Request.Graph) { OutError = FProtocolError::Make(EErrorCode::InvalidArgument, TEXT("Action search requires an exact context.graphGuid or context.graphName."), TEXT("FBlueprintSearch::Search")); OutError.AssetPath = AssetPath; return false; }
            FGraphActionSearchResult Result; FGraphActionError Error;
            if (!FBlueprintGraphActionCatalog::Search(Request, Result, Error)) { OutError = FProtocolError::Make(EErrorCode::ValidationFailed, Error.Message, Error.UECallsite); OutError.AssetPath = Error.AssetPath; return false; }
            OutResult = Result.ToJson(); return true;
        }

        TArray<TSharedPtr<FJsonValue>> Items;
        if (Domain == TEXT("asset") || Domain.IsEmpty())
        {
            FAssetRegistryModule& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")); TArray<FAssetData> Assets; Registry.Get().GetAllAssets(Assets, true);
            for (const FAssetData& Asset : Assets)
            {
                const FString Path = Asset.ObjectPath.ToString(); if (!Match(Path, Query) && !Match(Asset.AssetName.ToString(), Query)) continue;
                TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("domain"), TEXT("asset")); Item->SetStringField(TEXT("assetPath"), Path); Item->SetStringField(TEXT("packageName"), Asset.PackageName.ToString()); Item->SetStringField(TEXT("className"), Asset.AssetClass.ToString()); Items.Add(MakeShared<FJsonValueObject>(Item));
            }
        }
        if (Domain == TEXT("class"))
        {
            for (TObjectIterator<UClass> It; It; ++It) if (!It->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists) && Match(It->GetPathName(), Query)) { TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("domain"), TEXT("class")); Item->SetStringField(TEXT("classPath"), It->GetPathName()); Item->SetStringField(TEXT("superClassPath"), It->GetSuperClass() ? It->GetSuperClass()->GetPathName() : FString()); Items.Add(MakeShared<FJsonValueObject>(Item)); }
        }
        if (Domain == TEXT("member") || Domain == TEXT("property"))
        {
            FString ClassPath; if (Context.IsValid()) Context->TryGetStringField(TEXT("classPath"), ClassPath); UClass* Class = LoadObject<UClass>(nullptr, *ClassPath, nullptr, LOAD_NoWarn);
            if (!Class) { OutError = FProtocolError::Make(EErrorCode::InvalidArgument, TEXT("Member/property search requires context.classPath."), TEXT("FBlueprintSearch::Search")); return false; }
            for (TFieldIterator<FProperty> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It) if (Match(It->GetName(), Query)) { TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("domain"), TEXT("property")); Item->SetStringField(TEXT("name"), It->GetName()); Item->SetStringField(TEXT("ownerClassPath"), It->GetOwnerStruct()->GetPathName()); Item->SetStringField(TEXT("cppType"), It->GetCPPType()); Items.Add(MakeShared<FJsonValueObject>(Item)); }
            if (Domain == TEXT("member")) for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It) if (Match(It->GetName(), Query)) { TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("domain"), TEXT("function")); Item->SetStringField(TEXT("name"), It->GetName()); Item->SetStringField(TEXT("ownerClassPath"), It->GetOwnerClass()->GetPathName()); Items.Add(MakeShared<FJsonValueObject>(Item)); }
        }
        if (Items.Num() == 0 && Domain != TEXT("asset") && Domain != TEXT("class") && Domain != TEXT("member") && Domain != TEXT("property") && !Domain.IsEmpty()) { OutError = FProtocolError::Make(EErrorCode::InvalidArgument, FString::Printf(TEXT("Unknown search domain '%s'."), *Domain), TEXT("FBlueprintSearch::Search")); return false; }
        Items.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B) { FString AText, BText; TSharedPtr<FJsonObject> AO = A->AsObject(), BO = B->AsObject(); if (AO->TryGetStringField(TEXT("assetPath"), AText) || AO->TryGetStringField(TEXT("classPath"), AText) || AO->TryGetStringField(TEXT("name"), AText)) {} if (BO->TryGetStringField(TEXT("assetPath"), BText) || BO->TryGetStringField(TEXT("classPath"), BText) || BO->TryGetStringField(TEXT("name"), BText)) {} return AText < BText; });
        OutResult = Paged(Items, Offset, Limit); return true;
    }
}
