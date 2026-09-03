#include "PiUnrealBlueprintVerification.h"

#include "Engine/Blueprint.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "PackageTools.h"
#include "PiUnrealBlueprintInspection.h"
#include "UObject/Package.h"

namespace PiUnrealBlueprint
{
    bool FBlueprintVerification::Verify(const TArray<FString>& AssetPaths,
        const TArray<TSharedPtr<FJsonValue>>& Expectations, const bool bCompile, const bool bReload,
        TSharedRef<FJsonObject>& OutResult, FProtocolError& OutError)
    {
        if (!IsInGameThread()) { OutError = FProtocolError::Make(EErrorCode::InternalError, TEXT("Blueprint verification must run on the game thread."), TEXT("FBlueprintVerification::Verify")); return false; }
        if (AssetPaths.Num() == 0)
        {
            OutError = FProtocolError::Make(EErrorCode::InvalidArgument, TEXT("At least one asset path is required."), TEXT("FBlueprintVerification::Verify"));
            return false;
        }
        TSet<FString> UniqueAssetPaths;
        for (const FString& Path : AssetPaths)
        {
            if (Path.TrimStartAndEnd().IsEmpty() || UniqueAssetPaths.Contains(Path))
            {
                OutError = FProtocolError::Make(EErrorCode::InvalidArgument, TEXT("Asset paths must be non-empty and unique."), TEXT("FBlueprintVerification::Verify"));
                OutError.AssetPath = Path;
                return false;
            }
            UniqueAssetPaths.Add(Path);
        }
        TMap<FString, TSharedPtr<FJsonObject>> ExpectationsByAsset;
        static const TSet<FString> AllowedExpectationFields = {TEXT("assetPath"), TEXT("structureHash")};
        for (const TSharedPtr<FJsonValue>& Value : Expectations)
        {
            if (!Value.IsValid() || Value->Type != EJson::Object)
            {
                OutError = FProtocolError::Make(EErrorCode::TypeMismatch, TEXT("Every verification expectation must be an object."), TEXT("FBlueprintVerification::Verify"));
                return false;
            }
            const TSharedPtr<FJsonObject> Expected = Value->AsObject();
            FString ExpectedPath;
            if (!Expected->TryGetStringField(TEXT("assetPath"), ExpectedPath) || !UniqueAssetPaths.Contains(ExpectedPath)
                || ExpectationsByAsset.Contains(ExpectedPath))
            {
                OutError = FProtocolError::Make(EErrorCode::InvalidArgument,
                    TEXT("Each expectation must identify one requested assetPath exactly once."), TEXT("FBlueprintVerification::Verify"));
                OutError.AssetPath = ExpectedPath;
                return false;
            }
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Expected->Values)
            {
                if (!AllowedExpectationFields.Contains(Pair.Key))
                {
                    OutError = FProtocolError::Make(EErrorCode::UnknownField,
                        FString::Printf(TEXT("Verification expectation rejects unknown field '%s'."), *Pair.Key), TEXT("FBlueprintVerification::Verify"));
                    OutError.AssetPath = ExpectedPath;
                    return false;
                }
            }
            FString ExpectedHash;
            if (Expected->HasField(TEXT("structureHash"))
                && (!Expected->TryGetStringField(TEXT("structureHash"), ExpectedHash) || ExpectedHash.IsEmpty()))
            {
                OutError = FProtocolError::Make(EErrorCode::TypeMismatch,
                    TEXT("expectation.structureHash must be a non-empty string."), TEXT("FBlueprintVerification::Verify"));
                OutError.AssetPath = ExpectedPath;
                return false;
            }
            ExpectationsByAsset.Add(ExpectedPath, Expected);
        }
        TArray<UBlueprint*> Blueprints; TArray<UPackage*> Packages;
        for (const FString& Path : AssetPaths)
        {
            UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *Path, nullptr, LOAD_NoWarn);
            if (!Blueprint) { OutError = FProtocolError::Make(EErrorCode::AssetNotFound, FString::Printf(TEXT("Blueprint '%s' was not found."), *Path), TEXT("LoadObject<UBlueprint>")); OutError.AssetPath = Path; return false; }
            Blueprints.Add(Blueprint); Packages.AddUnique(Blueprint->GetOutermost());
        }
        if (bReload)
        {
            for (UPackage* Package : Packages) if (Package->IsDirty()) { OutError = FProtocolError::Make(EErrorCode::VerificationFailed, TEXT("Reload verification refuses a dirty package."), TEXT("FBlueprintVerification::Verify")); OutError.AssetPath = Package->GetName(); return false; }
            FText ReloadError; if (!UPackageTools::ReloadPackages(Packages, ReloadError, EReloadPackagesInteractionMode::AssumePositive)) { OutError = FProtocolError::Make(EErrorCode::VerificationFailed, ReloadError.IsEmpty() ? TEXT("Package reload failed.") : ReloadError.ToString(), TEXT("UPackageTools::ReloadPackages")); return false; }
            Blueprints.Reset(); for (const FString& Path : AssetPaths) { UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *Path, nullptr, LOAD_NoWarn); if (!Blueprint) { OutError = FProtocolError::Make(EErrorCode::AssetNotFound, TEXT("Blueprint disappeared after reload."), TEXT("LoadObject<UBlueprint>")); OutError.AssetPath = Path; return false; } Blueprints.Add(Blueprint); }
        }
        TArray<TSharedPtr<FJsonValue>> Items;
        for (int32 Index = 0; Index < Blueprints.Num(); ++Index)
        {
            UBlueprint* Blueprint = Blueprints[Index]; TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("assetPath"), Blueprint->GetPathName());
            TArray<FString> Messages;
            if (bCompile)
            {
                FCompilerResultsLog Log; Log.bSilentMode = true; FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &Log);
                for (const TSharedRef<FTokenizedMessage>& Message : Log.Messages) Messages.Add(Message->ToText().ToString());
                if (Log.NumErrors > 0 || !Blueprint->IsUpToDate()) { OutError = FProtocolError::Make(EErrorCode::VerificationFailed, TEXT("Blueprint compilation failed during verification."), TEXT("FKismetEditorUtilities::CompileBlueprint")); OutError.AssetPath = Blueprint->GetPathName(); OutError.CompilerMessages = Messages; return false; }
            }
            TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>(); FProtocolError InspectError;
            if (!FBlueprintInspection::Inspect(Blueprint->GetPathName(), {}, 0, 500, Snapshot, InspectError)) { OutError = InspectError; return false; }
            Item->SetStringField(TEXT("structureHash"), Snapshot->GetStringField(TEXT("structureHash"))); Item->SetBoolField(TEXT("compiled"), bCompile); Item->SetBoolField(TEXT("reloaded"), bReload);
            const TSharedPtr<FJsonObject> Expected = ExpectationsByAsset.FindRef(AssetPaths[Index]);
            if (Expected.IsValid())
            {
                FString ExpectedHash; if (Expected->TryGetStringField(TEXT("structureHash"), ExpectedHash) && !ExpectedHash.Equals(Snapshot->GetStringField(TEXT("structureHash")), ESearchCase::IgnoreCase)) { OutError = FProtocolError::Make(EErrorCode::VerificationFailed, TEXT("Blueprint structureHash does not match the expectation."), TEXT("FBlueprintVerification::Verify")); OutError.AssetPath = Blueprint->GetPathName(); return false; }
            }
            Items.Add(MakeShared<FJsonValueObject>(Item));
        }
        OutResult = MakeShared<FJsonObject>(); OutResult->SetBoolField(TEXT("verified"), true); OutResult->SetArrayField(TEXT("assets"), Items); return true;
    }
}
