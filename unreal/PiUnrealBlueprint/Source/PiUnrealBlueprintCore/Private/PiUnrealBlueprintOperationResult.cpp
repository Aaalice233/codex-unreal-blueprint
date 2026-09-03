#include "PiUnrealBlueprintOperationResult.h"

namespace PiUnrealBlueprint
{
    TSharedRef<FJsonObject> FBlueprintOperationError::ToJson() const
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("code"), Code);
        Json->SetStringField(TEXT("message"), Message);
        Json->SetStringField(TEXT("ueCallsite"), UECallsite);
        if (!AssetPath.IsEmpty())
        {
            Json->SetStringField(TEXT("assetPath"), AssetPath);
        }
        if (!GraphPath.IsEmpty())
        {
            Json->SetStringField(TEXT("graphPath"), GraphPath);
        }
        if (OperationIndex != INDEX_NONE)
        {
            Json->SetNumberField(TEXT("operationIndex"), OperationIndex);
        }
        if (Details.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> JsonDetails;
            for (const FString& Detail : Details)
            {
                JsonDetails.Add(MakeShared<FJsonValueString>(Detail));
            }
            Json->SetArrayField(TEXT("details"), JsonDetails);
        }
        if (Candidates.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> JsonCandidates;
            for (const FString& Candidate : Candidates)
            {
                JsonCandidates.Add(MakeShared<FJsonValueString>(Candidate));
            }
            Json->SetArrayField(TEXT("candidates"), JsonCandidates);
        }
        return Json;
    }

    FBlueprintOperationResult FBlueprintOperationResult::Success(const TArray<FString>& InAffectedAssets, const bool bInChanged)
    {
        FBlueprintOperationResult Result;
        Result.bSuccess = true;
        Result.bChanged = bInChanged;
        Result.AffectedAssets = InAffectedAssets;
        Result.Data = MakeShared<FJsonObject>();
        return Result;
    }

    FBlueprintOperationResult FBlueprintOperationResult::Failure(const FString& Code, const FString& Message,
        const FString& AssetPath, const int32 OperationIndex, const FString& UECallsite,
        const TArray<FString>& Details)
    {
        FBlueprintOperationResult Result;
        FBlueprintOperationError Error;
        Error.Code = Code;
        Error.Message = Message;
        Error.AssetPath = AssetPath;
        Error.OperationIndex = OperationIndex;
        Error.UECallsite = UECallsite;
        Error.Details = Details;
        Result.Error = MoveTemp(Error);
        Result.Data = MakeShared<FJsonObject>();
        return Result;
    }

    TSharedRef<FJsonObject> FBlueprintOperationResult::ToJson() const
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetBoolField(TEXT("success"), bSuccess);
        Json->SetBoolField(TEXT("changed"), bChanged);
        TArray<TSharedPtr<FJsonValue>> Assets;
        for (const FString& Asset : AffectedAssets)
        {
            Assets.Add(MakeShared<FJsonValueString>(Asset));
        }
        Json->SetArrayField(TEXT("affectedAssets"), Assets);
        Json->SetObjectField(TEXT("data"), Data.IsValid() ? Data.ToSharedRef() : MakeShared<FJsonObject>());
        if (Error.IsSet())
        {
            Json->SetObjectField(TEXT("error"), Error.GetValue().ToJson());
        }
        return Json;
    }
}
