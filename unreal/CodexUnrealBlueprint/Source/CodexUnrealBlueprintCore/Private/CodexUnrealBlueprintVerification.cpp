#include "CodexUnrealBlueprintVerification.h"

#include "Engine/Blueprint.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "PackageTools.h"
#include "CodexUnrealBlueprintComponentOperations.h"
#include "CodexUnrealBlueprintInspection.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"

namespace CodexUnrealBlueprint
{
    namespace
    {
        FString CanonicalJson(const TSharedPtr<FJsonValue>& Value)
        {
            if (!Value.IsValid()) return TEXT("null");
            FString Text;
            const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
            FJsonSerializer::Serialize(Value, TEXT(""), Writer);
            return Text;
        }

        bool JsonEqual(const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
        {
            return CanonicalJson(A) == CanonicalJson(B);
        }

        bool ReadVector(const TSharedPtr<FJsonValue>& Value, const bool bRotation, FVector& Out)
        {
            if (!Value.IsValid()) return false;
            FString Text;
            if (Value->TryGetString(Text))
            {
                if (!bRotation) return Out.InitFromString(Text);
                FRotator Rotation; if (!Rotation.InitFromString(Text)) return false;
                Out = FVector(Rotation.Pitch, Rotation.Yaw, Rotation.Roll); return true;
            }
            if (Value->Type != EJson::Object) return false;
            const TSharedPtr<FJsonObject> Object = Value->AsObject(); double A = 0.0, B = 0.0, C = 0.0;
            if (bRotation)
            {
                if (!Object->TryGetNumberField(TEXT("pitch"), A) || !Object->TryGetNumberField(TEXT("yaw"), B)
                    || !Object->TryGetNumberField(TEXT("roll"), C)) return false;
            }
            else if (!Object->TryGetNumberField(TEXT("x"), A) || !Object->TryGetNumberField(TEXT("y"), B)
                || !Object->TryGetNumberField(TEXT("z"), C)) return false;
            Out = FVector(A, B, C); return true;
        }

        bool TransformEqual(const TSharedPtr<FJsonValue>& Expected, const TSharedPtr<FJsonValue>& Actual)
        {
            if (!Expected.IsValid() || !Actual.IsValid() || Expected->Type != EJson::Object || Actual->Type != EJson::Object) return false;
            FVector ExpectedLocation, ExpectedRotation, ExpectedScale, ActualLocation, ActualRotation, ActualScale;
            const TSharedPtr<FJsonObject> E = Expected->AsObject(), A = Actual->AsObject();
            return ReadVector(E->TryGetField(TEXT("location")), false, ExpectedLocation)
                && ReadVector(E->TryGetField(TEXT("rotation")), true, ExpectedRotation)
                && ReadVector(E->TryGetField(TEXT("scale")), false, ExpectedScale)
                && ReadVector(A->TryGetField(TEXT("location")), false, ActualLocation)
                && ReadVector(A->TryGetField(TEXT("rotation")), true, ActualRotation)
                && ReadVector(A->TryGetField(TEXT("scale")), false, ActualScale)
                && ExpectedLocation.Equals(ActualLocation, KINDA_SMALL_NUMBER)
                && ExpectedRotation.Equals(ActualRotation, KINDA_SMALL_NUMBER)
                && ExpectedScale.Equals(ActualScale, KINDA_SMALL_NUMBER);
        }

        bool FailExpectation(FProtocolError& OutError, const FString& AssetPath, const int32 ExpectationIndex,
            const FString& ComponentName, const FString& PropertyPath, const TSharedPtr<FJsonValue>& Expected,
            const TSharedPtr<FJsonValue>& Actual)
        {
            OutError = FProtocolError::Make(EErrorCode::VerificationFailed,
                FString::Printf(TEXT("Expectation %d failed for component '%s', property '%s': expected %s, actual %s."),
                    ExpectationIndex, *ComponentName, *PropertyPath, *CanonicalJson(Expected), *CanonicalJson(Actual)),
                TEXT("FBlueprintVerification::Verify"));
            OutError.AssetPath = AssetPath;
            return false;
        }
    }

    bool FBlueprintVerification::Verify(const TArray<FString>& AssetPaths,
        const TArray<TSharedPtr<FJsonValue>>& Expectations, const bool bCompile, const bool bReload,
        TSharedRef<FJsonObject>& OutResult, FProtocolError& OutError,
        const TFunction<void(const FString&, int32, int32, const FString&)>& Progress)
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
        static const TSet<FString> AllowedExpectationFields = {
            TEXT("assetPath"), TEXT("structureHash"), TEXT("packageDirty"), TEXT("components"), TEXT("componentCounts")};
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
            bool ExpectedDirty = false;
            if (Expected->HasField(TEXT("packageDirty")) && !Expected->TryGetBoolField(TEXT("packageDirty"), ExpectedDirty))
            {
                OutError = FProtocolError::Make(EErrorCode::TypeMismatch,
                    TEXT("expectation.packageDirty must be boolean."), TEXT("FBlueprintVerification::Verify"));
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
            if (Progress) for (int32 Index = 0; Index < AssetPaths.Num(); ++Index) Progress(TEXT("Reload"), Index + 1, AssetPaths.Num(), AssetPaths[Index]);
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
                if (Progress) Progress(TEXT("Compile"), Index + 1, Blueprints.Num(), Blueprint->GetPathName());
            }
            TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>(); FProtocolError InspectError;
            if (!FBlueprintInspection::Inspect(Blueprint->GetPathName(), {}, 0, 500, Snapshot, InspectError)) { OutError = InspectError; return false; }
            Item->SetStringField(TEXT("structureHash"), Snapshot->GetStringField(TEXT("structureHash"))); Item->SetBoolField(TEXT("compiled"), bCompile); Item->SetBoolField(TEXT("reloaded"), bReload);
            const TSharedPtr<FJsonObject> Expected = ExpectationsByAsset.FindRef(AssetPaths[Index]);
            if (Expected.IsValid())
            {
                FString ExpectedHash; if (Expected->TryGetStringField(TEXT("structureHash"), ExpectedHash) && !ExpectedHash.Equals(Snapshot->GetStringField(TEXT("structureHash")), ESearchCase::IgnoreCase)) { OutError = FProtocolError::Make(EErrorCode::VerificationFailed, TEXT("Blueprint structureHash does not match the expectation."), TEXT("FBlueprintVerification::Verify")); OutError.AssetPath = Blueprint->GetPathName(); return false; }
                bool ExpectedDirty = false;
                if (Expected->TryGetBoolField(TEXT("packageDirty"), ExpectedDirty) && Blueprint->GetOutermost()->IsDirty() != ExpectedDirty)
                    return FailExpectation(OutError, Blueprint->GetPathName(), Index, TEXT(""), TEXT("packageDirty"),
                        MakeShared<FJsonValueBoolean>(ExpectedDirty), MakeShared<FJsonValueBoolean>(Blueprint->GetOutermost()->IsDirty()));

                const TArray<TSharedPtr<FJsonValue>>* ComponentCounts = nullptr;
                if (Expected->TryGetArrayField(TEXT("componentCounts"), ComponentCounts))
                {
                    for (int32 CountIndex = 0; CountIndex < ComponentCounts->Num(); ++CountIndex)
                    {
                        const TSharedPtr<FJsonObject> Count = (*ComponentCounts)[CountIndex]->AsObject();
                        double Equals = -1.0; if (!Count.IsValid() || !Count->TryGetNumberField(TEXT("equals"), Equals))
                            return FailExpectation(OutError, Blueprint->GetPathName(), CountIndex, TEXT(""), TEXT("componentCounts.equals"), (*ComponentCounts)[CountIndex], MakeShared<FJsonValueNull>());
                        TSharedRef<FJsonObject> Query = MakeShared<FJsonObject>();
                        for (const FString& Field : {TEXT("variableNames"), TEXT("variableNameRegex"), TEXT("classPaths"), TEXT("inherited")})
                            if (const TSharedPtr<FJsonValue>* Value = Count->Values.Find(Field)) Query->SetField(Field, *Value);
                        FBlueprintOperationResult Listed = FBlueprintComponentOperations::List(Blueprint, true, Query, CountIndex);
                        if (!Listed.bSuccess) { OutError = FProtocolError::Make(EErrorCode::VerificationFailed, TEXT("Component count query failed."), TEXT("FBlueprintComponentOperations::List")); OutError.AssetPath = Blueprint->GetPathName(); return false; }
                        const double Actual = Listed.Data->GetNumberField(TEXT("componentsMatched"));
                        if (Actual != Equals) return FailExpectation(OutError, Blueprint->GetPathName(), CountIndex, TEXT(""), TEXT("componentCounts.equals"), MakeShared<FJsonValueNumber>(Equals), MakeShared<FJsonValueNumber>(Actual));
                    }
                }

                const TArray<TSharedPtr<FJsonValue>>* ComponentExpectations = nullptr;
                if (Expected->TryGetArrayField(TEXT("components"), ComponentExpectations))
                {
                    for (int32 ComponentIndex = 0; ComponentIndex < ComponentExpectations->Num(); ++ComponentIndex)
                    {
                        const TSharedPtr<FJsonObject> ComponentExpected = (*ComponentExpectations)[ComponentIndex]->AsObject();
                        if (!ComponentExpected.IsValid()) return FailExpectation(OutError, Blueprint->GetPathName(), ComponentIndex, TEXT(""), TEXT("components"), (*ComponentExpectations)[ComponentIndex], MakeShared<FJsonValueNull>());
                        TArray<FString> Names; FString Name;
                        if (ComponentExpected->TryGetStringField(TEXT("variableName"), Name)) Names.Add(Name);
                        else
                        {
                            FString Pattern; double Start = 0.0, End = -1.0;
                            if (!ComponentExpected->TryGetStringField(TEXT("namePattern"), Pattern)
                                || !ComponentExpected->TryGetNumberField(TEXT("startIndex"), Start)
                                || !ComponentExpected->TryGetNumberField(TEXT("endIndex"), End) || Start > End || End - Start + 1 > 200)
                                return FailExpectation(OutError, Blueprint->GetPathName(), ComponentIndex, TEXT(""), TEXT("selector"), (*ComponentExpectations)[ComponentIndex], MakeShared<FJsonValueNull>());
                            for (int32 Number = static_cast<int32>(Start); Number <= static_cast<int32>(End); ++Number)
                                Names.Add(Pattern.Replace(TEXT("{index}"), *FString::FromInt(Number)));
                        }
                        const TSharedPtr<FJsonObject>* Properties = nullptr; TArray<FString> PropertyPaths;
                        if (ComponentExpected->TryGetObjectField(TEXT("properties"), Properties) && Properties) (*Properties)->Values.GetKeys(PropertyPaths);
                        TSharedRef<FJsonObject> Query = MakeShared<FJsonObject>();
                        TArray<TSharedPtr<FJsonValue>> NameValues; for (const FString& TargetName : Names) NameValues.Add(MakeShared<FJsonValueString>(TargetName));
                        Query->SetArrayField(TEXT("variableNames"), NameValues); Query->SetArrayField(TEXT("propertyPaths"), [&PropertyPaths]() { TArray<TSharedPtr<FJsonValue>> Values; for (const FString& Path : PropertyPaths) Values.Add(MakeShared<FJsonValueString>(Path)); return Values; }());
                        FBlueprintOperationResult Listed = FBlueprintComponentOperations::List(Blueprint, true, Query, ComponentIndex);
                        if (!Listed.bSuccess) { OutError = FProtocolError::Make(EErrorCode::VerificationFailed, TEXT("Component expectation query failed."), TEXT("FBlueprintComponentOperations::List")); OutError.AssetPath = Blueprint->GetPathName(); return false; }
                        const TArray<TSharedPtr<FJsonValue>>& ActualComponents = Listed.Data->GetArrayField(TEXT("components"));
                        for (const FString& TargetName : Names)
                        {
                            TSharedPtr<FJsonObject> Actual;
                            for (const TSharedPtr<FJsonValue>& Candidate : ActualComponents) if (Candidate->AsObject()->GetStringField(TEXT("variableName")) == TargetName) { Actual = Candidate->AsObject(); break; }
                            bool Exists = true; ComponentExpected->TryGetBoolField(TEXT("exists"), Exists);
                            if (Actual.IsValid() != Exists) return FailExpectation(OutError, Blueprint->GetPathName(), ComponentIndex, TargetName, TEXT("exists"), MakeShared<FJsonValueBoolean>(Exists), MakeShared<FJsonValueBoolean>(Actual.IsValid()));
                            if (!Actual.IsValid()) continue;
                            const TArray<TPair<FString, FString>> Mappings = {
                                TPair<FString,FString>(TEXT("classPath"),TEXT("componentClassPath")),
                                TPair<FString,FString>(TEXT("inherited"),TEXT("inherited")),
                                TPair<FString,FString>(TEXT("parentVariableName"),TEXT("parentVariableName")),
                                TPair<FString,FString>(TEXT("relativeTransform"),TEXT("relativeTransform"))};
                            for (const TPair<FString, FString>& Mapping : Mappings)
                            {
                                const TSharedPtr<FJsonValue>* ExpectedValue = ComponentExpected->Values.Find(Mapping.Key);
                                const TSharedPtr<FJsonValue> ActualValue = Actual->TryGetField(Mapping.Value);
                                const bool bMatches = Mapping.Key == TEXT("relativeTransform")
                                    ? TransformEqual(ExpectedValue ? *ExpectedValue : nullptr, ActualValue)
                                    : ExpectedValue && JsonEqual(*ExpectedValue, ActualValue);
                                if (ExpectedValue && (!Actual->HasField(Mapping.Value) || !bMatches))
                                    return FailExpectation(OutError, Blueprint->GetPathName(), ComponentIndex, TargetName, Mapping.Key, *ExpectedValue, ActualValue);
                            }
                            if (Properties && *Properties)
                            {
                                const TSharedPtr<FJsonObject>* ActualProperties = nullptr; Actual->TryGetObjectField(TEXT("properties"), ActualProperties);
                                for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Properties)->Values)
                                {
                                    const TSharedPtr<FJsonValue> ActualValue = ActualProperties && *ActualProperties ? (*ActualProperties)->TryGetField(Pair.Key) : nullptr;
                                    if (!ActualValue.IsValid() || !JsonEqual(Pair.Value, ActualValue)) return FailExpectation(OutError, Blueprint->GetPathName(), ComponentIndex, TargetName, Pair.Key, Pair.Value, ActualValue);
                                }
                            }
                        }
                    }
                }
            }
            Items.Add(MakeShared<FJsonValueObject>(Item));
            if (Progress) Progress(TEXT("Verify"), Index + 1, Blueprints.Num(), Blueprint->GetPathName());
        }
        OutResult = MakeShared<FJsonObject>(); OutResult->SetBoolField(TEXT("verified"), true); OutResult->SetArrayField(TEXT("assets"), Items); return true;
    }
}
