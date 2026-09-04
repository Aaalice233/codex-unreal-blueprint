#include "CodexUnrealBlueprintOperationRegistry.h"

#include "AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Animation/AnimBlueprint.h"
#include "WidgetBlueprint.h"
#include "Components/ActorComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/UserDefinedEnum.h"
#include "Engine/World.h"
#include "Engine/UserDefinedStruct.h"
#include "HAL/PlatformTime.h"
#include "Misc/PackageName.h"
#include "CodexUnrealBlueprintAnimOperations.h"
#include "CodexUnrealBlueprintAssetOperations.h"
#include "CodexUnrealBlueprintComponentOperations.h"
#include "CodexUnrealBlueprintGraphOperations.h"
#include "CodexUnrealBlueprintTypeSystem.h"
#include "CodexUnrealBlueprintUmgOperations.h"
#include "UObject/ObjectRedirector.h"

#include <initializer_list>

namespace CodexUnrealBlueprint
{
    namespace
    {
        enum class EFieldKind : uint8 { String, Number, Boolean, Object, Array, Any };

        struct FFieldSpec
        {
            const TCHAR* Name;
            EFieldKind Kind;
            bool bRequired;
        };

        TSharedRef<FJsonObject> TypeSchema(const EFieldKind Kind)
        {
            TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
            if (Kind != EFieldKind::Any)
            {
                const TCHAR* Type = Kind == EFieldKind::String ? TEXT("string")
                    : Kind == EFieldKind::Number ? TEXT("number")
                    : Kind == EFieldKind::Boolean ? TEXT("boolean")
                    : Kind == EFieldKind::Object ? TEXT("object") : TEXT("array");
                Json->SetStringField(TEXT("type"), Type);
            }
            return Json;
        }

        TSharedRef<FJsonObject> MakeSchema(const FString& Operation, const TArray<FFieldSpec>& Fields)
        {
            TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
            Schema->SetStringField(TEXT("type"), TEXT("object"));
            Schema->SetBoolField(TEXT("additionalProperties"), false);
            TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
            TSharedRef<FJsonObject> Discriminator = TypeSchema(EFieldKind::String);
            Discriminator->SetStringField(TEXT("const"), Operation);
            Properties->SetObjectField(TEXT("operation"), Discriminator);
            TArray<TSharedPtr<FJsonValue>> Required;
            Required.Add(MakeShared<FJsonValueString>(TEXT("operation")));
            for (const FFieldSpec& Field : Fields)
            {
                Properties->SetObjectField(Field.Name, TypeSchema(Field.Kind));
                if (Field.bRequired) Required.Add(MakeShared<FJsonValueString>(Field.Name));
            }
            Schema->SetObjectField(TEXT("properties"), Properties);
            Schema->SetArrayField(TEXT("required"), Required);
            return Schema;
        }

        TSharedRef<FJsonObject> StringEnumSchema(std::initializer_list<const TCHAR*> Values)
        {
            TSharedRef<FJsonObject> Schema = TypeSchema(EFieldKind::String);
            TArray<TSharedPtr<FJsonValue>> Enum;
            for (const TCHAR* Value : Values) Enum.Add(MakeShared<FJsonValueString>(Value));
            Schema->SetArrayField(TEXT("enum"), Enum);
            return Schema;
        }

        TSharedRef<FJsonObject> NonEmptyStringSchema()
        {
            TSharedRef<FJsonObject> Schema = TypeSchema(EFieldKind::String);
            Schema->SetNumberField(TEXT("minLength"), 1);
            return Schema;
        }

        TSharedRef<FJsonObject> IntegerSchema(const int32 Minimum = MIN_int32)
        {
            TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
            Schema->SetStringField(TEXT("type"), TEXT("integer"));
            if (Minimum != MIN_int32) Schema->SetNumberField(TEXT("minimum"), Minimum);
            return Schema;
        }

        TSharedRef<FJsonObject> ExactObjectSchema(const TMap<FString, TSharedPtr<FJsonValue>>& Properties,
            std::initializer_list<const TCHAR*> Required)
        {
            TSharedRef<FJsonObject> Schema = TypeSchema(EFieldKind::Object);
            Schema->SetBoolField(TEXT("additionalProperties"), false);
            TSharedRef<FJsonObject> PropertyObject = MakeShared<FJsonObject>();
            PropertyObject->Values = Properties;
            Schema->SetObjectField(TEXT("properties"), PropertyObject);
            TArray<TSharedPtr<FJsonValue>> RequiredValues;
            for (const TCHAR* Name : Required) RequiredValues.Add(MakeShared<FJsonValueString>(Name));
            Schema->SetArrayField(TEXT("required"), RequiredValues);
            return Schema;
        }

        void SetFieldSchema(TMap<FString, FOperationDefinition>& Definitions, const TCHAR* Operation,
            const TCHAR* Field, const TSharedRef<FJsonObject>& FieldSchema)
        {
            FOperationDefinition* Definition = Definitions.Find(Operation);
            if (!Definition) return;
            const TSharedPtr<FJsonObject>* Properties = nullptr;
            if (Definition->Schema->TryGetObjectField(TEXT("properties"), Properties) && Properties
                && (*Properties)->HasField(Field))
                (*Properties)->SetObjectField(Field, FieldSchema);
        }

        bool ValidateSchemaValue(const TSharedPtr<FJsonValue>& Value, const FJsonObject& Schema,
            const FString& FieldPath, FString& OutMessage)
        {
            auto CountMatchingBranches = [&](const TCHAR* Keyword) -> int32
            {
                const TArray<TSharedPtr<FJsonValue>>* Branches = nullptr;
                if (!Schema.TryGetArrayField(Keyword, Branches) || !Branches) return INDEX_NONE;
                int32 Matches = 0;
                for (const TSharedPtr<FJsonValue>& Branch : *Branches)
                {
                    const TSharedPtr<FJsonObject>* BranchSchema = nullptr;
                    FString Ignored;
                    if (Branch.IsValid() && Branch->TryGetObject(BranchSchema) && BranchSchema
                        && ValidateSchemaValue(Value, **BranchSchema, FieldPath, Ignored)) ++Matches;
                }
                return Matches;
            };
            const int32 AnyOfMatches = CountMatchingBranches(TEXT("anyOf"));
            if (AnyOfMatches != INDEX_NONE && AnyOfMatches == 0)
            {
                OutMessage = FString::Printf(TEXT("Field '%s' does not match any allowed schema."), *FieldPath);
                return false;
            }
            const int32 OneOfMatches = CountMatchingBranches(TEXT("oneOf"));
            if (OneOfMatches != INDEX_NONE && OneOfMatches != 1)
            {
                OutMessage = FString::Printf(TEXT("Field '%s' must match exactly one allowed schema."), *FieldPath);
                return false;
            }

            FString Type;
            Schema.TryGetStringField(TEXT("type"), Type);
            const bool bTypeMatches = Type.IsEmpty()
                || (Type == TEXT("string") && Value.IsValid() && Value->Type == EJson::String)
                || ((Type == TEXT("number") || Type == TEXT("integer")) && Value.IsValid() && Value->Type == EJson::Number)
                || (Type == TEXT("boolean") && Value.IsValid() && Value->Type == EJson::Boolean)
                || (Type == TEXT("object") && Value.IsValid() && Value->Type == EJson::Object)
                || (Type == TEXT("array") && Value.IsValid() && Value->Type == EJson::Array);
            if (!bTypeMatches)
            {
                OutMessage = FString::Printf(TEXT("Field '%s' must be %s."), *FieldPath, *Type);
                return false;
            }
            if (Type == TEXT("integer") && Value->AsNumber() != FMath::RoundToDouble(Value->AsNumber()))
            {
                OutMessage = FString::Printf(TEXT("Field '%s' must be an integer."), *FieldPath);
                return false;
            }
            double Minimum = 0.0;
            if ((Type == TEXT("number") || Type == TEXT("integer")) && Schema.TryGetNumberField(TEXT("minimum"), Minimum)
                && Value->AsNumber() < Minimum)
            {
                OutMessage = FString::Printf(TEXT("Field '%s' must be at least %g."), *FieldPath, Minimum);
                return false;
            }
            if (Type == TEXT("string"))
            {
                double MinLength = 0.0;
                if (Schema.TryGetNumberField(TEXT("minLength"), MinLength) && Value->AsString().Len() < MinLength)
                {
                    OutMessage = FString::Printf(TEXT("Field '%s' must not be empty."), *FieldPath);
                    return false;
                }
                FString Constant;
                if (Schema.TryGetStringField(TEXT("const"), Constant) && Value->AsString() != Constant)
                {
                    OutMessage = FString::Printf(TEXT("Field '%s' must equal '%s'."), *FieldPath, *Constant);
                    return false;
                }
                const TArray<TSharedPtr<FJsonValue>>* Enum = nullptr;
                if (Schema.TryGetArrayField(TEXT("enum"), Enum) && Enum)
                {
                    bool bFound = false;
                    for (const TSharedPtr<FJsonValue>& Candidate : *Enum)
                        bFound = bFound || (Candidate.IsValid() && Candidate->Type == EJson::String && Candidate->AsString() == Value->AsString());
                    if (!bFound)
                    {
                        OutMessage = FString::Printf(TEXT("Field '%s' has an unsupported value."), *FieldPath);
                        return false;
                    }
                }
            }
            if (Type == TEXT("object"))
            {
                const TSharedPtr<FJsonObject> Object = Value->AsObject();
                const TArray<TSharedPtr<FJsonValue>>* Required = nullptr;
                if (Schema.TryGetArrayField(TEXT("required"), Required) && Required)
                {
                    for (const TSharedPtr<FJsonValue>& RequiredField : *Required)
                    {
                        if (!Object->HasField(RequiredField->AsString()))
                        {
                            OutMessage = FString::Printf(TEXT("Field '%s.%s' is required."), *FieldPath, *RequiredField->AsString());
                            return false;
                        }
                    }
                }
                const TSharedPtr<FJsonObject>* Properties = nullptr;
                Schema.TryGetObjectField(TEXT("properties"), Properties);
                bool bAdditionalProperties = true;
                Schema.TryGetBoolField(TEXT("additionalProperties"), bAdditionalProperties);
                const TSharedPtr<FJsonObject>* AdditionalPropertySchema = nullptr;
                Schema.TryGetObjectField(TEXT("additionalProperties"), AdditionalPropertySchema);
                for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
                {
                    const TSharedPtr<FJsonObject>* PropertySchema = nullptr;
                    if (!Properties || !(*Properties)->TryGetObjectField(Pair.Key, PropertySchema) || !PropertySchema)
                    {
                        if (AdditionalPropertySchema && *AdditionalPropertySchema)
                        {
                            if (!ValidateSchemaValue(Pair.Value, **AdditionalPropertySchema,
                                FieldPath + TEXT(".") + Pair.Key, OutMessage)) return false;
                            continue;
                        }
                        if (!bAdditionalProperties)
                        {
                            OutMessage = FString::Printf(TEXT("Unknown field '%s.%s'."), *FieldPath, *Pair.Key);
                            return false;
                        }
                        continue;
                    }
                    if (!ValidateSchemaValue(Pair.Value, **PropertySchema, FieldPath + TEXT(".") + Pair.Key, OutMessage)) return false;
                }
            }
            if (Type == TEXT("array"))
            {
                double MinItems = 0.0;
                if (Schema.TryGetNumberField(TEXT("minItems"), MinItems) && Value->AsArray().Num() < MinItems)
                {
                    OutMessage = FString::Printf(TEXT("Field '%s' has too few items."), *FieldPath);
                    return false;
                }
                const TSharedPtr<FJsonObject>* ItemSchema = nullptr;
                if (Schema.TryGetObjectField(TEXT("items"), ItemSchema) && ItemSchema)
                {
                    for (int32 Index = 0; Index < Value->AsArray().Num(); ++Index)
                        if (!ValidateSchemaValue(Value->AsArray()[Index], **ItemSchema,
                            FString::Printf(TEXT("%s[%d]"), *FieldPath, Index), OutMessage)) return false;
                }
            }
            return true;
        }

        FProtocolError OperationError(const EErrorCode Code, const FString& Message,
            const FString& Callsite, const int32 Index = INDEX_NONE, const FString& AssetPath = FString())
        {
            FProtocolError Error = FProtocolError::Make(Code, Message, Callsite);
            Error.OperationIndex = Index;
            Error.AssetPath = AssetPath;
            return Error;
        }

        FString ExactObjectPath(const FString& Path)
        {
            if (Path.Contains(TEXT("."))) return Path;
            const FString AssetName = FPackageName::GetLongPackageAssetName(Path);
            return AssetName.IsEmpty() ? Path : Path + TEXT(".") + AssetName;
        }

        UObject* LoadExactObject(const FString& Path)
        {
            const FString ObjectPath = ExactObjectPath(Path);
            return ObjectPath.IsEmpty() ? nullptr : StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath, nullptr, LOAD_NoWarn);
        }

        UClass* LoadExactClass(const FString& Path)
        {
            return LoadObject<UClass>(nullptr, *Path, nullptr, LOAD_NoWarn);
        }

        bool ReadString(const FJsonObject& Json, const TCHAR* Name, FString& Out)
        {
            return Json.TryGetStringField(Name, Out) && !Out.TrimStartAndEnd().IsEmpty();
        }

        void AddPackageImpact(const FString& ObjectOrPackagePath, const bool bCompile,
            FPreflightRequest& Request, const int32 OperationIndex, TArray<FString>* OutPackageNames = nullptr)
        {
            const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectOrPackagePath);
            if (!FPackageName::IsValidLongPackageName(PackageName, true)) return;
            Request.TargetPackageNames.AddUnique(PackageName);
            Request.OperationIndicesByPackage.FindOrAdd(PackageName).AddUnique(OperationIndex);
            if (bCompile) Request.CompilePackageNames.AddUnique(PackageName);
            if (OutPackageNames) OutPackageNames->AddUnique(PackageName);
        }

        enum class EBlueprintReferencerCompileScope : uint8
        {
            AllBlueprintReferencers,
            DerivedBlueprintsOnly
        };

        void FindDerivedBlueprintPackages(const FString& ObjectOrPackagePath, IAssetRegistry& Registry,
            TSet<FString>& OutPackageNames)
        {
            const FAssetData TargetAsset = Registry.GetAssetByObjectPath(FName(*ObjectOrPackagePath));
            if (!TargetAsset.IsValid()) return;
            const FName GeneratedClassPath = TargetAsset.GetTagValueRef<FName>(FBlueprintTags::GeneratedClassPath);
            if (GeneratedClassPath.IsNone()) return;

            TArray<FName> ParentClassPaths = {GeneratedClassPath};
            TSet<FName> VisitedClassPaths;
            for (int32 ParentIndex = 0; ParentIndex < ParentClassPaths.Num(); ++ParentIndex)
            {
                const FName ParentClassPath = ParentClassPaths[ParentIndex];
                if (ParentClassPath.IsNone() || VisitedClassPaths.Contains(ParentClassPath)) continue;
                VisitedClassPaths.Add(ParentClassPath);

                FARFilter Filter;
                Filter.TagsAndValues.Add(FBlueprintTags::ParentClassPath, ParentClassPath.ToString());
                TArray<FAssetData> Children;
                if (!Registry.GetAssets(Filter, Children)) continue;
                for (const FAssetData& Child : Children)
                {
                    OutPackageNames.Add(Child.PackageName.ToString());
                    const FName ChildClassPath = Child.GetTagValueRef<FName>(FBlueprintTags::GeneratedClassPath);
                    if (!ChildClassPath.IsNone() && !VisitedClassPaths.Contains(ChildClassPath))
                        ParentClassPaths.AddUnique(ChildClassPath);
                }
            }
        }

        void AddReferencerImpacts(const FString& ObjectOrPackagePath, FPreflightRequest& Request,
            const int32 OperationIndex, const EBlueprintReferencerCompileScope CompileScope,
            TArray<FString>* OutPackageNames = nullptr)
        {
            const double StartedAt = FPlatformTime::Seconds();
            const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectOrPackagePath);
            if (!FPackageName::IsValidLongPackageName(PackageName, true)) return;
            FAssetRegistryModule& RegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
            IAssetRegistry& Registry = RegistryModule.Get();
            TArray<FName> Referencers;
            Registry.GetReferencers(FName(*PackageName), Referencers,
                UE::AssetRegistry::EDependencyCategory::Package);
            Request.AssetRegistryReferencerCount += Referencers.Num();
            TSet<FString> DerivedBlueprintPackages;
            if (CompileScope == EBlueprintReferencerCompileScope::DerivedBlueprintsOnly)
                FindDerivedBlueprintPackages(ObjectOrPackagePath, Registry, DerivedBlueprintPackages);
            for (const FName Referencer : Referencers)
            {
                const FString ReferencerPackage = Referencer.ToString();
                Request.AdditionalImpactPackageNames.AddUnique(ReferencerPackage);
                Request.OperationIndicesByPackage.FindOrAdd(ReferencerPackage).AddUnique(OperationIndex);
                Request.ReferencedFromByPackage.FindOrAdd(ReferencerPackage).AddUnique(ObjectOrPackagePath);
                TArray<FAssetData> Assets;
                Registry.GetAssetsByPackageName(Referencer, Assets, true);
                const bool bBlueprintPackage = Assets.ContainsByPredicate([](const FAssetData& Asset)
                {
                    return Asset.AssetClass.ToString().Contains(TEXT("Blueprint"));
                });
                const bool bCompilePackage = CompileScope == EBlueprintReferencerCompileScope::DerivedBlueprintsOnly
                    ? DerivedBlueprintPackages.Contains(ReferencerPackage)
                    : bBlueprintPackage;
                if (bCompilePackage) Request.CompilePackageNames.AddUnique(ReferencerPackage);
                if (OutPackageNames) OutPackageNames->AddUnique(ReferencerPackage);
            }
            for (const FString& DerivedPackage : DerivedBlueprintPackages)
            {
                Request.AdditionalImpactPackageNames.AddUnique(DerivedPackage);
                Request.CompilePackageNames.AddUnique(DerivedPackage);
                Request.OperationIndicesByPackage.FindOrAdd(DerivedPackage).AddUnique(OperationIndex);
                Request.ReferencedFromByPackage.FindOrAdd(DerivedPackage).AddUnique(ObjectOrPackagePath);
                if (OutPackageNames) OutPackageNames->AddUnique(DerivedPackage);
            }
            Request.ImpactDiscoveryDurationMs += (FPlatformTime::Seconds() - StartedAt) * 1000.0;
        }

        bool RequiresReferencerImpacts(const FString& OperationName)
        {
            // A class-default value change does not alter the Blueprint's public structure. Loading and compiling
            // every referencer can synchronously pull an entire project's hard-reference graph into the Editor.
            return OperationName != TEXT("asset.classDefault.set");
        }

        EBlueprintReferencerCompileScope ReferencerCompileScope(const FString& OperationName)
        {
            // Component mutations affect the target class and its inheritance chain. Other Blueprints that merely
            // hold a class/resource reference must remain reference-only or ResourceMap aggregators can expand a
            // small edit into a project-wide compile and load operation.
            return OperationName.StartsWith(TEXT("component."))
                ? EBlueprintReferencerCompileScope::DerivedBlueprintsOnly
                : EBlueprintReferencerCompileScope::AllBlueprintReferencers;
        }

        bool ParseComponentReference(const TSharedPtr<FJsonObject>& Json, FComponentReference& Out)
        {
            if (!Json.IsValid()) return false;
            FString Name;
            FString Guid;
            Json->TryGetStringField(TEXT("variableName"), Name);
            Json->TryGetStringField(TEXT("nodeGuid"), Guid);
            Json->TryGetStringField(TEXT("ownerBlueprintPath"), Out.OwnerBlueprintPath);
            Json->TryGetBoolField(TEXT("inherited"), Out.bInherited);
            Out.VariableName = FName(*Name);
            if (!Guid.IsEmpty() && !FGuid::Parse(Guid, Out.NodeGuid)) return false;
            return !Out.VariableName.IsNone() || Out.NodeGuid.IsValid();
        }

        bool ParseVectorField(const TSharedPtr<FJsonObject>& Json, const TCHAR* Field, FVector& Out)
        {
            FString Text;
            if (Json->TryGetStringField(Field, Text)) return Out.InitFromString(Text);
            const TSharedPtr<FJsonObject>* Value = nullptr;
            double X = 0.0, Y = 0.0, Z = 0.0;
            return Json->TryGetObjectField(Field, Value) && Value && (*Value)->TryGetNumberField(TEXT("x"), X)
                && (*Value)->TryGetNumberField(TEXT("y"), Y) && (*Value)->TryGetNumberField(TEXT("z"), Z)
                && (Out = FVector(X, Y, Z), true);
        }

        bool ParseRotationField(const TSharedPtr<FJsonObject>& Json, FRotator& Out)
        {
            FString Text;
            if (Json->TryGetStringField(TEXT("rotation"), Text)) return Out.InitFromString(Text);
            const TSharedPtr<FJsonObject>* Value = nullptr;
            double Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
            return Json->TryGetObjectField(TEXT("rotation"), Value) && Value
                && (*Value)->TryGetNumberField(TEXT("pitch"), Pitch) && (*Value)->TryGetNumberField(TEXT("yaw"), Yaw)
                && (*Value)->TryGetNumberField(TEXT("roll"), Roll) && (Out = FRotator(Pitch, Yaw, Roll), true);
        }

        bool ParseTransform(const TSharedPtr<FJsonObject>& Json, FTransform& Out)
        {
            if (!Json.IsValid()) return false;
            FVector Translation;
            FRotator Rotator;
            FVector Scale3D;
            if (!ParseVectorField(Json, TEXT("location"), Translation) || !ParseRotationField(Json, Rotator)
                || !ParseVectorField(Json, TEXT("scale"), Scale3D)) return false;
            Out = FTransform(Rotator, Translation, Scale3D);
            return true;
        }

        void CopyOperationError(const FBlueprintOperationError& Source, FWritePipelineError& Out)
        {
            Out.Code = Source.Code;
            Out.Message = Source.Message;
            Out.AssetPath = Source.AssetPath;
            Out.OperationIndex = Source.OperationIndex;
            Out.UECallsite = Source.UECallsite;
        }

        class FRegistryWriteOperation final : public IWriteOperation
        {
        public:
            FRegistryWriteOperation(const TSharedRef<FJsonObject>& InOperation, const int32 InIndex)
                : Operation(InOperation), Index(InIndex) {}

            virtual int32 GetOperationIndex() const override { return Index; }

            virtual void GatherPreflight(FPreflightRequest& Request) const override
            {
                FString OperationName;
                Operation->TryGetStringField(TEXT("operation"), OperationName);
                FString Path;
                if (Operation->TryGetStringField(OperationName == TEXT("asset.create") ? TEXT("packagePath") : TEXT("assetPath"), Path))
                {
                    AddPackageImpact(Path, OperationName != TEXT("asset.delete"), Request, Index);
                    if (OperationName != TEXT("asset.create") && RequiresReferencerImpacts(OperationName))
                        AddReferencerImpacts(Path, Request, Index, ReferencerCompileScope(OperationName));
                }
                if (Operation->TryGetStringField(TEXT("destinationPath"), Path)) AddPackageImpact(Path, true, Request, Index);

                FString ReferencePath;
                FString ExpectedClassPath;
                if (OperationName == TEXT("anim.skeleton.set") && Operation->TryGetStringField(TEXT("skeletonPath"), ReferencePath))
                    ExpectedClassPath = TEXT("/Script/Engine.Skeleton");
                else if ((OperationName == TEXT("anim.parent.set") || OperationName == TEXT("asset.parent.set")
                    || OperationName == TEXT("asset.create")) && Operation->TryGetStringField(TEXT("parentClassPath"), ReferencePath))
                    ExpectedClassPath = UClass::StaticClass()->GetPathName();
                else if ((OperationName == TEXT("asset.interface.add") || OperationName == TEXT("asset.interface.remove"))
                    && Operation->TryGetStringField(TEXT("interfaceClassPath"), ReferencePath))
                    ExpectedClassPath = UClass::StaticClass()->GetPathName();
                else if ((OperationName == TEXT("component.add") || OperationName == TEXT("widget.add")
                    || OperationName == TEXT("animation.track.add"))
                    && Operation->TryGetStringField(TEXT("classPath"), ReferencePath))
                    ExpectedClassPath = UClass::StaticClass()->GetPathName();
                if (!ReferencePath.IsEmpty())
                {
                    FTypeReferenceRequirement Requirement;
                    Requirement.ObjectPath = ReferencePath;
                    Requirement.ExpectedClassPath = ExpectedClassPath;
                    Requirement.OperationIndex = Index;
                    Request.TypeReferences.Add(MoveTemp(Requirement));
                }
            }

            virtual bool Apply(FWriteMutationContext& Context, FWritePipelineError& OutError) override
            {
                FString Name;
                FString AssetPath;
                Operation->TryGetStringField(TEXT("operation"), Name);
                Operation->TryGetStringField(TEXT("assetPath"), AssetPath);
                UObject* Asset = AssetPath.IsEmpty() ? nullptr : LoadExactObject(AssetPath);
                TArray<FString> ImpactPackageNames;
                FPreflightRequest RuntimeImpacts;
                if (!AssetPath.IsEmpty())
                {
                    AddPackageImpact(AssetPath, Name != TEXT("asset.delete"), RuntimeImpacts, Index, &ImpactPackageNames);
                    if (RequiresReferencerImpacts(Name))
                        AddReferencerImpacts(AssetPath, RuntimeImpacts, Index, ReferencerCompileScope(Name), &ImpactPackageNames);
                }
                FString DestinationPath;
                if (Operation->TryGetStringField(TEXT("destinationPath"), DestinationPath))
                    AddPackageImpact(DestinationPath, true, RuntimeImpacts, Index, &ImpactPackageNames);

                if (Name.StartsWith(TEXT("anim.")))
                {
                    UAnimBlueprint* AnimBlueprint = Cast<UAnimBlueprint>(Asset);
                    if (!AnimBlueprint)
                    {
                        OutError.Code = TEXT("ANIM_ASSET_TYPE_MISMATCH");
                        OutError.Message = TEXT("assetPath must identify a UAnimBlueprint for anim.* operations.");
                        OutError.AssetPath = AssetPath;
                        OutError.OperationIndex = Index;
                        OutError.UECallsite = TEXT("Cast<UAnimBlueprint>");
                        return false;
                    }
                    FAnimOperationResult Result;
                    FAnimOperationError Error;
                    if (!FBlueprintAnimOperations::Apply(AnimBlueprint, Operation, Context, Result, Error, Index))
                    {
                        OutError.Code = Error.Code; OutError.Message = Error.Message; OutError.AssetPath = Error.AssetPath;
                        OutError.OperationIndex = Index; OutError.UECallsite = Error.UECallsite; return false;
                    }
                    return true;
                }
                if (Name.StartsWith(TEXT("widget.")) || Name.StartsWith(TEXT("namedSlot."))
                    || Name.StartsWith(TEXT("slot.")) || Name.StartsWith(TEXT("binding."))
                    || Name.StartsWith(TEXT("navigation.")) || Name.StartsWith(TEXT("accessibility."))
                    || Name.StartsWith(TEXT("animation.")) || Name.StartsWith(TEXT("event.")))
                {
                    UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Asset);
                    if (!WidgetBlueprint)
                    {
                        OutError.Code = TEXT("UMG_ASSET_TYPE_MISMATCH");
                        OutError.Message = TEXT("assetPath must identify a UWidgetBlueprint for UMG operations.");
                        OutError.AssetPath = AssetPath;
                        OutError.OperationIndex = Index;
                        OutError.UECallsite = TEXT("Cast<UWidgetBlueprint>");
                        return false;
                    }
                    FUmgOperationResult Result;
                    FUmgOperationError Error;
                    if (!FBlueprintUmgOperations::Apply(WidgetBlueprint, Operation, Context, Result, Error, Index))
                    {
                        OutError.Code = Error.Code; OutError.Message = Error.Message; OutError.AssetPath = Error.AssetPath;
                        OutError.OperationIndex = Index; OutError.UECallsite = Error.UECallsite; return false;
                    }
                    return true;
                }
                if (Name.StartsWith(TEXT("graph.")) || Name.StartsWith(TEXT("node."))
                    || Name.StartsWith(TEXT("pin.")) || Name.StartsWith(TEXT("link."))
                    || Name.StartsWith(TEXT("signature.")) || Name.StartsWith(TEXT("local."))
                    || Name.StartsWith(TEXT("dispatcher.")))
                {
                    UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
                    if (!Context.Modify(Blueprint, OutError)) return false;
                    FGraphOperationResult Result;
                    FGraphActionError Error;
                    if (!FBlueprintGraphOperations::Apply(Blueprint, Operation, Result, Error, Index))
                    {
                        OutError.Code = Error.Code; OutError.Message = Error.Message; OutError.AssetPath = Error.AssetPath;
                        OutError.OperationIndex = Index; OutError.UECallsite = Error.UECallsite; return false;
                    }
                    Context.MarkPackageChanged(Blueprint->GetOutermost());
                    return true;
                }

                FBlueprintOperationResult Result;
                if (Name != TEXT("asset.create") && Name != TEXT("asset.duplicate")
                    && !Context.Modify(Asset, OutError)) return false;
                if (Name == TEXT("asset.create"))
                {
                    FString Path, KindText, ParentPath;
                    Operation->TryGetStringField(TEXT("packagePath"), Path);
                    Operation->TryGetStringField(TEXT("kind"), KindText);
                    Operation->TryGetStringField(TEXT("parentClassPath"), ParentPath);
                    EBlueprintAssetKind Kind = EBlueprintAssetKind::Blueprint;
                    if (KindText == TEXT("interface")) Kind = EBlueprintAssetKind::Interface;
                    else if (KindText == TEXT("functionLibrary")) Kind = EBlueprintAssetKind::FunctionLibrary;
                    else if (KindText == TEXT("macroLibrary")) Kind = EBlueprintAssetKind::MacroLibrary;
                    else if (KindText == TEXT("struct")) Kind = EBlueprintAssetKind::UserDefinedStruct;
                    else if (KindText == TEXT("enum")) Kind = EBlueprintAssetKind::UserDefinedEnum;
                    Result = FBlueprintAssetOperations::Create(Path, Kind, ParentPath.IsEmpty() ? nullptr : LoadExactClass(ParentPath), Index);
                }
                else if (Name == TEXT("asset.duplicate")) { FString Path; Operation->TryGetStringField(TEXT("destinationPath"), Path); Result = FBlueprintAssetOperations::Duplicate(Asset, Path, Index); }
                else if (Name == TEXT("asset.rename")) { FString Path; Operation->TryGetStringField(TEXT("destinationPath"), Path); Result = FBlueprintAssetOperations::RenameOrMove(Asset, Path, Index); }
                else if (Name == TEXT("asset.delete")) Result = FBlueprintAssetOperations::Delete(Asset, Index);
                else if (Name == TEXT("asset.parent.set")) { FString Path; Operation->TryGetStringField(TEXT("parentClassPath"), Path); Result = FBlueprintAssetOperations::ResetParent(Cast<UBlueprint>(Asset), LoadExactClass(Path), Index); }
                else if (Name == TEXT("asset.interface.add") || Name == TEXT("asset.interface.remove"))
                {
                    FString Path; bool bPreserve = false; Operation->TryGetStringField(TEXT("interfaceClassPath"), Path); Operation->TryGetBoolField(TEXT("preserveFunctions"), bPreserve);
                    Result = Name.EndsWith(TEXT("add")) ? FBlueprintAssetOperations::AddInterface(Cast<UBlueprint>(Asset), LoadExactClass(Path), Index)
                        : FBlueprintAssetOperations::RemoveInterface(Cast<UBlueprint>(Asset), LoadExactClass(Path), bPreserve, Index);
                }
                else if (Name == TEXT("asset.redirector.fix")) Result = FBlueprintAssetOperations::FixRedirector(Cast<UObjectRedirector>(Asset), Index);
                else if (Name == TEXT("asset.levelBlueprint.getOrCreate")) Result = FBlueprintAssetOperations::GetOrCreateLevelBlueprint(Cast<UWorld>(Asset), Index);
                else if (Name == TEXT("asset.classDefault.set")) { FString Path; Operation->TryGetStringField(TEXT("propertyPath"), Path); Result = FBlueprintAssetOperations::SetClassDefault(Cast<UBlueprint>(Asset), Path, Operation->TryGetField(TEXT("value")), Index); }
                else if (Name.StartsWith(TEXT("component."))) Result = ApplyComponent(Name, Cast<UBlueprint>(Asset));
                else if (Name.StartsWith(TEXT("variable."))) Result = ApplyVariable(Name, Cast<UBlueprint>(Asset));
                else if (Name.StartsWith(TEXT("struct."))) Result = ApplyStruct(Name, Cast<UUserDefinedStruct>(Asset));
                else if (Name.StartsWith(TEXT("enum."))) Result = ApplyEnum(Name, Cast<UUserDefinedEnum>(Asset));
                else { OutError.Code = TEXT("OPERATION_DISPATCH_MISSING"); OutError.Message = TEXT("Registry operation has no executor."); OutError.OperationIndex = Index; OutError.UECallsite = TEXT("FRegistryWriteOperation::Apply"); return false; }

                if (!Result.bSuccess)
                {
                    if (Result.Error.IsSet()) CopyOperationError(Result.Error.GetValue(), OutError);
                    else { OutError.Code = TEXT("OPERATION_FAILED"); OutError.Message = TEXT("Operation failed without structured error."); OutError.OperationIndex = Index; OutError.UECallsite = TEXT("FRegistryWriteOperation::Apply"); }
                    return false;
                }
                Context.RecordOperationResult(Index, Result.Data);
                for (const FString& AffectedPath : Result.AffectedAssets)
                {
                    const FString PackageName = FPackageName::ObjectPathToPackageName(AffectedPath);
                    if (FPackageName::IsValidLongPackageName(PackageName, true)) ImpactPackageNames.AddUnique(PackageName);
                }
                for (const FString& PackageName : ImpactPackageNames)
                {
                    UPackage* Package = FindPackage(nullptr, *PackageName);
                    if (Package && Package->IsDirty()) Context.MarkPackageChanged(Package);
                }
                return true;
            }

            virtual bool VerifyInMemory(FWritePipelineError& OutError) const override
            {
                FString Name;
                FString SourcePath;
                Operation->TryGetStringField(TEXT("operation"), Name);
                Operation->TryGetStringField(Name == TEXT("asset.create") ? TEXT("packagePath") : TEXT("assetPath"), SourcePath);
                if (Name == TEXT("asset.delete"))
                {
                    if (LoadExactObject(SourcePath) == nullptr) return true;
                    OutError.Code = TEXT("VERIFY_ASSET_STILL_EXISTS");
                    OutError.Message = TEXT("Deleted asset is still available after mutation.");
                    OutError.AssetPath = SourcePath;
                    OutError.OperationIndex = Index;
                    OutError.UECallsite = TEXT("StaticLoadObject");
                    return false;
                }

                FString ResultPath = SourcePath;
                if (Name == TEXT("asset.duplicate") || Name == TEXT("asset.rename"))
                    Operation->TryGetStringField(TEXT("destinationPath"), ResultPath);
                if (!ResultPath.IsEmpty() && LoadExactObject(ResultPath) == nullptr)
                {
                    OutError.Code = TEXT("VERIFY_ASSET_NOT_FOUND");
                    OutError.Message = TEXT("Affected asset is unavailable after mutation.");
                    OutError.AssetPath = ResultPath;
                    OutError.OperationIndex = Index;
                    OutError.UECallsite = TEXT("StaticLoadObject");
                    return false;
                }
                return true;
            }

        private:
            FBlueprintOperationResult ApplyComponent(const FString& Name, UBlueprint* Blueprint) const
            {
                const TSharedPtr<FJsonObject>* RefJson = nullptr;
                FComponentReference Ref;
                if (Name != TEXT("component.add") && Name != TEXT("component.cloneRange")
                    && (!Operation->TryGetObjectField(TEXT("component"), RefJson) || !ParseComponentReference(*RefJson, Ref)))
                    return FBlueprintOperationResult::Failure(TEXT("COMPONENT_REFERENCE_INVALID"), TEXT("component must identify variableName or nodeGuid."), Blueprint ? Blueprint->GetPathName() : FString(), Index, TEXT("ParseComponentReference"));
                if (Name == TEXT("component.add"))
                {
                    FString ClassPath, VariableName; Operation->TryGetStringField(TEXT("classPath"), ClassPath); Operation->TryGetStringField(TEXT("variableName"), VariableName);
                    FTransform Transform = FTransform::Identity; const TSharedPtr<FJsonObject>* TransformJson = nullptr; if (Operation->TryGetObjectField(TEXT("transform"), TransformJson)) ParseTransform(*TransformJson, Transform);
                    TOptional<FComponentReference> Parent; const TSharedPtr<FJsonObject>* ParentJson = nullptr; FComponentReference ParentValue; if (Operation->TryGetObjectField(TEXT("parent"), ParentJson) && ParseComponentReference(*ParentJson, ParentValue)) Parent = ParentValue;
                    FBlueprintOperationResult Added = FBlueprintComponentOperations::Add(Blueprint, LoadExactClass(ClassPath), FName(*VariableName), Parent, Transform, Index);
                    const TSharedPtr<FJsonObject>* InitialProperties = nullptr;
                    if (Added.bSuccess && Operation->TryGetObjectField(TEXT("initialProperties"), InitialProperties) && InitialProperties)
                    {
                        FComponentReference AddedRef; AddedRef.VariableName = FName(*VariableName);
                        for (const TPair<FString, TSharedPtr<FJsonValue>>& Property : (*InitialProperties)->Values)
                        {
                            FBlueprintOperationResult Set = FBlueprintComponentOperations::SetProperty(Blueprint, AddedRef,
                                Property.Key, Property.Value, false, Index);
                            if (!Set.bSuccess) return Set;
                        }
                    }
                    return Added;
                }
                if (Name == TEXT("component.cloneRange"))
                {
                    const TSharedPtr<FJsonObject>* SourceJson = nullptr; FComponentReference Source;
                    FString Pattern; double Start = 0.0, End = -1.0; const TSharedPtr<FJsonObject>* Overrides = nullptr;
                    if (!Operation->TryGetObjectField(TEXT("sourceComponent"), SourceJson) || !ParseComponentReference(*SourceJson, Source)
                        || !Operation->TryGetStringField(TEXT("targetPattern"), Pattern)
                        || !Operation->TryGetNumberField(TEXT("startIndex"), Start) || !Operation->TryGetNumberField(TEXT("endIndex"), End))
                        return FBlueprintOperationResult::Failure(TEXT("COMPONENT_CLONE_RANGE_INVALID"), TEXT("cloneRange fields are invalid."),
                            Blueprint ? Blueprint->GetPathName() : FString(), Index, TEXT("FRegistryWriteOperation::ApplyComponent"));
                    Operation->TryGetObjectField(TEXT("propertyOverrides"), Overrides);
                    return FBlueprintComponentOperations::CloneRange(Blueprint, Source, Pattern, static_cast<int32>(Start),
                        static_cast<int32>(End), Overrides ? *Overrides : nullptr, Index);
                }
                if (Name == TEXT("component.remove")) { bool Value = false; Operation->TryGetBoolField(TEXT("promoteChildren"), Value); return FBlueprintComponentOperations::Remove(Blueprint, Ref, Value, Index); }
                if (Name == TEXT("component.rename")) { FString Value; Operation->TryGetStringField(TEXT("newName"), Value); return FBlueprintComponentOperations::Rename(Blueprint, Ref, FName(*Value), Index); }
                if (Name == TEXT("component.attach")) { TOptional<FComponentReference> Parent; const TSharedPtr<FJsonObject>* Json = nullptr; FComponentReference Value; if (Operation->TryGetObjectField(TEXT("parent"), Json) && ParseComponentReference(*Json, Value)) Parent = Value; return FBlueprintComponentOperations::Attach(Blueprint, Ref, Parent, Index); }
                if (Name == TEXT("component.root.set")) return FBlueprintComponentOperations::SetRoot(Blueprint, Ref, Index);
                if (Name == TEXT("component.transform.set")) { const TSharedPtr<FJsonObject>* Json = nullptr; FTransform Value; if (!Operation->TryGetObjectField(TEXT("transform"), Json) || !ParseTransform(*Json, Value)) return FBlueprintOperationResult::Failure(TEXT("TRANSFORM_INVALID"), TEXT("transform requires location/scale as Unreal text or {x,y,z}, and rotation as Unreal text or {pitch,yaw,roll}."), Blueprint ? Blueprint->GetPathName() : FString(), Index, TEXT("ParseTransform")); return FBlueprintComponentOperations::SetTransform(Blueprint, Ref, Value, Index); }
                if (Name == TEXT("component.property.set")) { FString Path; bool Override = false; Operation->TryGetStringField(TEXT("propertyPath"), Path); Operation->TryGetBoolField(TEXT("createInheritedOverride"), Override); return FBlueprintComponentOperations::SetProperty(Blueprint, Ref, Path, Operation->TryGetField(TEXT("value")), Override, Index); }
                return FBlueprintComponentOperations::ClearInheritedOverride(Blueprint, Ref, Index);
            }

            FBlueprintOperationResult ApplyVariable(const FString& Name, UBlueprint* Blueprint) const
            {
                FString Existing; Operation->TryGetStringField(TEXT("existingName"), Existing);
                if (Name == TEXT("variable.remove")) { FString Value; Operation->TryGetStringField(TEXT("name"), Value); return FBlueprintTypeSystem::RemoveVariable(Blueprint, FName(*Value), Index); }
                FBlueprintVariableDefinition Definition; FString Value; Operation->TryGetStringField(TEXT("name"), Value); Definition.Name = FName(*Value);
                const TSharedPtr<FJsonObject>* Type = nullptr; FBlueprintOperationError Error;
                if (!Operation->TryGetObjectField(TEXT("type"), Type) || !FBlueprintTypeSystem::ParsePinType(*Type, Definition.Type, Error, Blueprint ? Blueprint->GetPathName() : FString(), Index)) { FBlueprintOperationResult Result; Result.Error = Error; return Result; }
                Definition.DefaultValue = Operation->TryGetField(TEXT("default")); Operation->TryGetStringField(TEXT("category"), Definition.Category); Operation->TryGetStringField(TEXT("tooltip"), Definition.Tooltip);
                Operation->TryGetBoolField(TEXT("instanceEditable"), Definition.bInstanceEditable); Operation->TryGetBoolField(TEXT("exposeOnSpawn"), Definition.bExposeOnSpawn); Operation->TryGetBoolField(TEXT("private"), Definition.bPrivate); Operation->TryGetBoolField(TEXT("saveGame"), Definition.bSaveGame); Operation->TryGetBoolField(TEXT("advancedDisplay"), Definition.bAdvancedDisplay); Operation->TryGetBoolField(TEXT("transient"), Definition.bTransient); Operation->TryGetBoolField(TEXT("replicated"), Definition.bReplicated); Operation->TryGetBoolField(TEXT("repNotify"), Definition.bRepNotify);
                FString RepNotifyFunction; Operation->TryGetStringField(TEXT("repNotifyFunction"), RepNotifyFunction); Definition.RepNotifyFunction = FName(*RepNotifyFunction);
                if (Definition.bRepNotify && Definition.RepNotifyFunction.IsNone())
                    return FBlueprintOperationResult::Failure(TEXT("VARIABLE_REPNOTIFY_FUNCTION_REQUIRED"), TEXT("repNotify=true requires repNotifyFunction."), Blueprint ? Blueprint->GetPathName() : FString(), Index, TEXT("FRegistryWriteOperation::ApplyVariable"));
                const TSharedPtr<FJsonObject>* Metadata = nullptr;
                if (Operation->TryGetObjectField(TEXT("metadata"), Metadata) && Metadata)
                {
                    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Metadata)->Values)
                    {
                        FString MetadataValue;
                        if (Pair.Value.IsValid() && Pair.Value->TryGetString(MetadataValue)) Definition.Metadata.Add(FName(*Pair.Key), MetadataValue);
                    }
                }
                return Name == TEXT("variable.add") ? FBlueprintTypeSystem::AddVariable(Blueprint, Definition, Index) : FBlueprintTypeSystem::UpdateVariable(Blueprint, FName(*Existing), Definition, Index);
            }

            FBlueprintOperationResult ApplyStruct(const FString& Name, UUserDefinedStruct* Struct) const
            {
                FString GuidText, DisplayName, DefaultValue, Tooltip; Operation->TryGetStringField(TEXT("fieldGuid"), GuidText); Operation->TryGetStringField(TEXT("displayName"), DisplayName); Operation->TryGetStringField(TEXT("defaultValue"), DefaultValue); Operation->TryGetStringField(TEXT("tooltip"), Tooltip); FGuid Guid; FGuid::Parse(GuidText, Guid);
                if (Name == TEXT("struct.field.remove")) return FBlueprintAssetOperations::RemoveStructField(Struct, Guid, Index);
                const TSharedPtr<FJsonObject>* Type = nullptr; FEdGraphPinType PinType; FBlueprintOperationError Error; if (!Operation->TryGetObjectField(TEXT("type"), Type) || !FBlueprintTypeSystem::ParsePinType(*Type, PinType, Error, Struct ? Struct->GetPathName() : FString(), Index)) { FBlueprintOperationResult Result; Result.Error = Error; return Result; }
                return Name == TEXT("struct.field.add") ? FBlueprintAssetOperations::AddStructField(Struct, DisplayName, PinType, DefaultValue, Tooltip, Index) : FBlueprintAssetOperations::UpdateStructField(Struct, Guid, DisplayName, PinType, DefaultValue, Tooltip, Index);
            }

            FBlueprintOperationResult ApplyEnum(const FString& Name, UUserDefinedEnum* Enum) const
            {
                double ValueIndex = 0; FString DisplayName; bool Bitflags = false; Operation->TryGetNumberField(TEXT("valueIndex"), ValueIndex); Operation->TryGetStringField(TEXT("displayName"), DisplayName); Operation->TryGetBoolField(TEXT("bitflags"), Bitflags);
                if (Name == TEXT("enum.value.add")) return FBlueprintAssetOperations::AddEnumValue(Enum, DisplayName, Index);
                if (Name == TEXT("enum.value.rename")) return FBlueprintAssetOperations::RenameEnumValue(Enum, static_cast<int32>(ValueIndex), DisplayName, Index);
                if (Name == TEXT("enum.value.remove")) return FBlueprintAssetOperations::RemoveEnumValue(Enum, static_cast<int32>(ValueIndex), Index);
                return FBlueprintAssetOperations::SetEnumBitflags(Enum, Bitflags, Index);
            }

            TSharedRef<FJsonObject> Operation;
            int32 Index;
        };

        TArray<FFieldSpec> CommonFields(const FString& Name)
        {
            TArray<FFieldSpec> Fields;
            if (Name == TEXT("asset.create")) Fields = {{TEXT("packagePath"), EFieldKind::String, true}, {TEXT("kind"), EFieldKind::String, true}, {TEXT("parentClassPath"), EFieldKind::String, false}};
            else Fields.Add({TEXT("assetPath"), EFieldKind::String, true});
            return Fields;
        }

        void AddFields(TArray<FFieldSpec>& Fields, const TArray<FFieldSpec>& Extra)
        {
            for (const FFieldSpec& Field : Extra) Fields.Add(Field);
        }
    }

    FOperationRegistry& FOperationRegistry::Get() { static FOperationRegistry Registry; return Registry; }

    FOperationRegistry::FOperationRegistry()
    {
        auto Add = [this](const FString& Name, const FString& Domain, const TArray<FFieldSpec>& Extra)
        {
            TArray<FFieldSpec> Fields = CommonFields(Name); AddFields(Fields, Extra);
            TSharedRef<FJsonObject> Example = MakeShared<FJsonObject>(); Example->SetStringField(TEXT("operation"), Name);
            for (const FFieldSpec& Field : Fields)
            {
                if (!Field.bRequired) continue;
                if (Field.Kind == EFieldKind::String) Example->SetStringField(Field.Name, FCString::Strcmp(Field.Name, TEXT("assetPath")) == 0 ? TEXT("/Game/Blueprints/BP_Example.BP_Example") : TEXT("value"));
                else if (Field.Kind == EFieldKind::Number) Example->SetNumberField(Field.Name, 0);
                else if (Field.Kind == EFieldKind::Boolean) Example->SetBoolField(Field.Name, false);
                else if (Field.Kind == EFieldKind::Array) Example->SetArrayField(Field.Name, {});
                else if (Field.Kind == EFieldKind::Object) Example->SetObjectField(Field.Name, MakeShared<FJsonObject>());
                else Example->SetField(Field.Name, MakeShared<FJsonValueNull>());
            }
            FOperationDefinition Definition{Name, Domain, FString::Printf(TEXT("Execute %s through the UE4.27 Core write pipeline."), *Name), MakeSchema(Name, Fields), Example};
            Definitions.Add(Name, MoveTemp(Definition));
        };
#define CODEX_ADD(Name, Domain, ...) Add(TEXT(Name), TEXT(Domain), {__VA_ARGS__})
        Add(TEXT("asset.create"), TEXT("asset"), {});
        CODEX_ADD("asset.duplicate", "asset", {TEXT("destinationPath"), EFieldKind::String, true}); CODEX_ADD("asset.rename", "asset", {TEXT("destinationPath"), EFieldKind::String, true}); CODEX_ADD("asset.delete", "asset");
        CODEX_ADD("asset.parent.set", "asset", {TEXT("parentClassPath"), EFieldKind::String, true}); CODEX_ADD("asset.interface.add", "asset", {TEXT("interfaceClassPath"), EFieldKind::String, true}); CODEX_ADD("asset.interface.remove", "asset", {TEXT("interfaceClassPath"), EFieldKind::String, true}, {TEXT("preserveFunctions"), EFieldKind::Boolean, false}); CODEX_ADD("asset.redirector.fix", "asset"); CODEX_ADD("asset.levelBlueprint.getOrCreate", "asset"); CODEX_ADD("asset.classDefault.set", "asset", {TEXT("propertyPath"), EFieldKind::String, true}, {TEXT("value"), EFieldKind::Any, true});
        CODEX_ADD("component.add", "component", {TEXT("classPath"), EFieldKind::String, true}, {TEXT("variableName"), EFieldKind::String, true}, {TEXT("parent"), EFieldKind::Object, false}, {TEXT("transform"), EFieldKind::Object, false}, {TEXT("initialProperties"), EFieldKind::Object, false});
        CODEX_ADD("component.cloneRange", "component", {TEXT("sourceComponent"), EFieldKind::Object, true}, {TEXT("targetPattern"), EFieldKind::String, true}, {TEXT("startIndex"), EFieldKind::Number, true}, {TEXT("endIndex"), EFieldKind::Number, true}, {TEXT("propertyOverrides"), EFieldKind::Object, false});
        CODEX_ADD("component.remove", "component", {TEXT("component"), EFieldKind::Object, true}, {TEXT("promoteChildren"), EFieldKind::Boolean, false}); CODEX_ADD("component.rename", "component", {TEXT("component"), EFieldKind::Object, true}, {TEXT("newName"), EFieldKind::String, true}); CODEX_ADD("component.attach", "component", {TEXT("component"), EFieldKind::Object, true}, {TEXT("parent"), EFieldKind::Object, false}); CODEX_ADD("component.root.set", "component", {TEXT("component"), EFieldKind::Object, true}); CODEX_ADD("component.transform.set", "component", {TEXT("component"), EFieldKind::Object, true}, {TEXT("transform"), EFieldKind::Object, true}); CODEX_ADD("component.property.set", "component", {TEXT("component"), EFieldKind::Object, true}, {TEXT("propertyPath"), EFieldKind::String, true}, {TEXT("value"), EFieldKind::Any, true}, {TEXT("createInheritedOverride"), EFieldKind::Boolean, false}); CODEX_ADD("component.override.clear", "component", {TEXT("component"), EFieldKind::Object, true});
        const TArray<FFieldSpec> VariableFields = {{TEXT("name"), EFieldKind::String, true}, {TEXT("type"), EFieldKind::Object, true}, {TEXT("default"), EFieldKind::Any, false}, {TEXT("category"), EFieldKind::String, false}, {TEXT("tooltip"), EFieldKind::String, false}, {TEXT("metadata"), EFieldKind::Object, false}, {TEXT("instanceEditable"), EFieldKind::Boolean, false}, {TEXT("exposeOnSpawn"), EFieldKind::Boolean, false}, {TEXT("private"), EFieldKind::Boolean, false}, {TEXT("saveGame"), EFieldKind::Boolean, false}, {TEXT("advancedDisplay"), EFieldKind::Boolean, false}, {TEXT("transient"), EFieldKind::Boolean, false}, {TEXT("replicated"), EFieldKind::Boolean, false}, {TEXT("repNotify"), EFieldKind::Boolean, false}, {TEXT("repNotifyFunction"), EFieldKind::String, false}};
        Add(TEXT("variable.add"), TEXT("type"), VariableFields);
        TArray<FFieldSpec> VariableUpdateFields = VariableFields; VariableUpdateFields.Add({TEXT("existingName"), EFieldKind::String, true});
        Add(TEXT("variable.update"), TEXT("type"), VariableUpdateFields);
        CODEX_ADD("variable.remove", "type", {TEXT("name"), EFieldKind::String, true});
        CODEX_ADD("struct.field.add", "type", {TEXT("displayName"), EFieldKind::String, true}, {TEXT("type"), EFieldKind::Object, true}, {TEXT("defaultValue"), EFieldKind::String, false}, {TEXT("tooltip"), EFieldKind::String, false}); CODEX_ADD("struct.field.update", "type", {TEXT("fieldGuid"), EFieldKind::String, true}, {TEXT("displayName"), EFieldKind::String, true}, {TEXT("type"), EFieldKind::Object, true}, {TEXT("defaultValue"), EFieldKind::String, false}, {TEXT("tooltip"), EFieldKind::String, false}); CODEX_ADD("struct.field.remove", "type", {TEXT("fieldGuid"), EFieldKind::String, true}); CODEX_ADD("enum.value.add", "type", {TEXT("displayName"), EFieldKind::String, true}); CODEX_ADD("enum.value.rename", "type", {TEXT("valueIndex"), EFieldKind::Number, true}, {TEXT("displayName"), EFieldKind::String, true}); CODEX_ADD("enum.value.remove", "type", {TEXT("valueIndex"), EFieldKind::Number, true}); CODEX_ADD("enum.bitflags.set", "type", {TEXT("bitflags"), EFieldKind::Boolean, true});

        const TArray<FFieldSpec> GraphSelector = {
            {TEXT("graphGuid"), EFieldKind::String, false}, {TEXT("graphName"), EFieldKind::String, false}
        };
        auto GraphFields = [&GraphSelector](const FString& Name, const TArray<FFieldSpec>& Specific)
        {
            TArray<FFieldSpec> Fields;
            if (Name != TEXT("graph.add") && Name != TEXT("dispatcher.add") && Name != TEXT("dispatcher.remove"))
                Fields.Append(GraphSelector);
            Fields.Append(Specific);
            return Fields;
        };
        Add(TEXT("graph.add"), TEXT("graph"), GraphFields(TEXT("graph.add"), {{TEXT("kind"),EFieldKind::String,true},{TEXT("name"),EFieldKind::String,true}}));
        Add(TEXT("graph.remove"), TEXT("graph"), GraphFields(TEXT("graph.remove"), {}));
        Add(TEXT("signature.set"), TEXT("graph"), GraphFields(TEXT("signature.set"), {{TEXT("inputs"),EFieldKind::Array,false},{TEXT("outputs"),EFieldKind::Array,false},{TEXT("signatureNodeGuid"),EFieldKind::String,false}}));
        Add(TEXT("local.add"), TEXT("graph"), GraphFields(TEXT("local.add"), {{TEXT("name"),EFieldKind::String,true},{TEXT("type"),EFieldKind::Object,true},{TEXT("defaultValue"),EFieldKind::String,false}}));
        Add(TEXT("local.remove"), TEXT("graph"), GraphFields(TEXT("local.remove"), {{TEXT("name"),EFieldKind::String,true}}));
        Add(TEXT("dispatcher.add"), TEXT("graph"), GraphFields(TEXT("dispatcher.add"), {{TEXT("name"),EFieldKind::String,true}}));
        Add(TEXT("dispatcher.remove"), TEXT("graph"), GraphFields(TEXT("dispatcher.remove"), {{TEXT("name"),EFieldKind::String,true}}));
        Add(TEXT("node.spawn"), TEXT("graph"), GraphFields(TEXT("node.spawn"), {{TEXT("actionId"),EFieldKind::String,true},{TEXT("customEventName"),EFieldKind::String,false},{TEXT("x"),EFieldKind::Number,true},{TEXT("y"),EFieldKind::Number,true}}));
        Add(TEXT("node.copy"), TEXT("graph"), GraphFields(TEXT("node.copy"), {{TEXT("nodeGuids"),EFieldKind::Array,true},{TEXT("offsetX"),EFieldKind::Number,false},{TEXT("offsetY"),EFieldKind::Number,false}}));
        Add(TEXT("node.comment"), TEXT("graph"), GraphFields(TEXT("node.comment"), {{TEXT("x"),EFieldKind::Number,true},{TEXT("y"),EFieldKind::Number,true},{TEXT("text"),EFieldKind::String,false},{TEXT("width"),EFieldKind::Number,false},{TEXT("height"),EFieldKind::Number,false}}));
        Add(TEXT("node.reroute"), TEXT("graph"), GraphFields(TEXT("node.reroute"), {{TEXT("x"),EFieldKind::Number,true},{TEXT("y"),EFieldKind::Number,true}}));
        Add(TEXT("node.delete"), TEXT("graph"), GraphFields(TEXT("node.delete"), {{TEXT("nodeGuid"),EFieldKind::String,true}}));
        Add(TEXT("node.move"), TEXT("graph"), GraphFields(TEXT("node.move"), {{TEXT("nodeGuid"),EFieldKind::String,true},{TEXT("x"),EFieldKind::Number,true},{TEXT("y"),EFieldKind::Number,true}}));
        Add(TEXT("node.reconstruct"), TEXT("graph"), GraphFields(TEXT("node.reconstruct"), {{TEXT("nodeGuid"),EFieldKind::String,true}}));
        Add(TEXT("node.refresh"), TEXT("graph"), GraphFields(TEXT("node.refresh"), {{TEXT("nodeGuid"),EFieldKind::String,true}}));
        Add(TEXT("pin.default"), TEXT("graph"), GraphFields(TEXT("pin.default"), {{TEXT("nodeGuid"),EFieldKind::String,true},{TEXT("pinGuid"),EFieldKind::String,true},{TEXT("value"),EFieldKind::String,true},{TEXT("valueKind"),EFieldKind::String,false}}));
        Add(TEXT("link.connect"), TEXT("graph"), GraphFields(TEXT("link.connect"), {{TEXT("aNodeGuid"),EFieldKind::String,true},{TEXT("aPinGuid"),EFieldKind::String,true},{TEXT("bNodeGuid"),EFieldKind::String,true},{TEXT("bPinGuid"),EFieldKind::String,true}}));
        Add(TEXT("link.disconnect"), TEXT("graph"), GraphFields(TEXT("link.disconnect"), {{TEXT("aNodeGuid"),EFieldKind::String,true},{TEXT("aPinGuid"),EFieldKind::String,true},{TEXT("bNodeGuid"),EFieldKind::String,true},{TEXT("bPinGuid"),EFieldKind::String,true}}));
        Add(TEXT("graph.layout"), TEXT("graph"), GraphFields(TEXT("graph.layout"), {{TEXT("nodeGuids"),EFieldKind::Array,false}}));

        Add(TEXT("widget.add"), TEXT("umg"), {{TEXT("name"),EFieldKind::String,true},{TEXT("classPath"),EFieldKind::String,true},{TEXT("parent"),EFieldKind::String,false},{TEXT("namedSlot"),EFieldKind::String,false},{TEXT("childIndex"),EFieldKind::Number,false},{TEXT("properties"),EFieldKind::Object,false},{TEXT("slotProperties"),EFieldKind::Object,false}});
        Add(TEXT("widget.remove"), TEXT("umg"), {{TEXT("widget"),EFieldKind::String,true}});
        Add(TEXT("widget.rename"), TEXT("umg"), {{TEXT("widget"),EFieldKind::String,true},{TEXT("newName"),EFieldKind::String,true}});
        Add(TEXT("widget.reparent"), TEXT("umg"), {{TEXT("widget"),EFieldKind::String,true},{TEXT("parent"),EFieldKind::String,false},{TEXT("namedSlot"),EFieldKind::String,false},{TEXT("childIndex"),EFieldKind::Number,false},{TEXT("slotProperties"),EFieldKind::Object,false}});
        Add(TEXT("namedSlot.set"), TEXT("umg"), {{TEXT("parent"),EFieldKind::String,true},{TEXT("namedSlot"),EFieldKind::String,true},{TEXT("widget"),EFieldKind::String,true}});
        Add(TEXT("namedSlot.clear"), TEXT("umg"), {{TEXT("parent"),EFieldKind::String,true},{TEXT("namedSlot"),EFieldKind::String,true}});
        Add(TEXT("slot.property.set"), TEXT("umg"), {{TEXT("widget"),EFieldKind::String,true},{TEXT("property"),EFieldKind::String,true},{TEXT("value"),EFieldKind::Any,true}});
        Add(TEXT("widget.property.set"), TEXT("umg"), {{TEXT("widget"),EFieldKind::String,true},{TEXT("property"),EFieldKind::String,true},{TEXT("value"),EFieldKind::Any,true}});
        Add(TEXT("widget.variable.set"), TEXT("umg"), {{TEXT("widget"),EFieldKind::String,true},{TEXT("isVariable"),EFieldKind::Boolean,true}});
        Add(TEXT("event.bind"), TEXT("umg"), {{TEXT("widget"),EFieldKind::String,true},{TEXT("event"),EFieldKind::String,true},{TEXT("x"),EFieldKind::Number,false},{TEXT("y"),EFieldKind::Number,false}});
        Add(TEXT("event.unbind"), TEXT("umg"), {{TEXT("widget"),EFieldKind::String,true},{TEXT("event"),EFieldKind::String,true}});
        Add(TEXT("binding.set"), TEXT("umg"), {{TEXT("widget"),EFieldKind::String,true},{TEXT("property"),EFieldKind::String,true},{TEXT("kind"),EFieldKind::String,true},{TEXT("source"),EFieldKind::String,true}});
        Add(TEXT("binding.remove"), TEXT("umg"), {{TEXT("widget"),EFieldKind::String,true},{TEXT("property"),EFieldKind::String,true}});
        Add(TEXT("navigation.set"), TEXT("umg"), {{TEXT("widget"),EFieldKind::String,true},{TEXT("direction"),EFieldKind::String,true},{TEXT("rule"),EFieldKind::String,true},{TEXT("target"),EFieldKind::String,false}});
        Add(TEXT("navigation.clear"), TEXT("umg"), {{TEXT("widget"),EFieldKind::String,true},{TEXT("direction"),EFieldKind::String,true}});
        Add(TEXT("accessibility.set"), TEXT("umg"), {{TEXT("widget"),EFieldKind::String,true},{TEXT("properties"),EFieldKind::Object,true}});
        Add(TEXT("animation.add"), TEXT("umg"), {{TEXT("name"),EFieldKind::String,true},{TEXT("tickResolution"),EFieldKind::Number,false},{TEXT("displayRate"),EFieldKind::Number,false},{TEXT("startFrame"),EFieldKind::Number,false},{TEXT("endFrame"),EFieldKind::Number,false}});
        Add(TEXT("animation.remove"), TEXT("umg"), {{TEXT("animation"),EFieldKind::String,true}});
        Add(TEXT("animation.rename"), TEXT("umg"), {{TEXT("animation"),EFieldKind::String,true},{TEXT("newName"),EFieldKind::String,true}});
        Add(TEXT("animation.binding.add"), TEXT("umg"), {{TEXT("animation"),EFieldKind::String,true},{TEXT("widget"),EFieldKind::String,true},{TEXT("target"),EFieldKind::String,false}});
        Add(TEXT("animation.binding.remove"), TEXT("umg"), {{TEXT("animation"),EFieldKind::String,true},{TEXT("bindingGuid"),EFieldKind::String,true}});
        Add(TEXT("animation.track.add"), TEXT("umg"), {{TEXT("animation"),EFieldKind::String,true},{TEXT("bindingGuid"),EFieldKind::String,true},{TEXT("classPath"),EFieldKind::String,true},{TEXT("propertyName"),EFieldKind::String,false},{TEXT("propertyPath"),EFieldKind::String,false}});
        Add(TEXT("animation.track.remove"), TEXT("umg"), {{TEXT("animation"),EFieldKind::String,true},{TEXT("bindingGuid"),EFieldKind::String,true},{TEXT("trackIndex"),EFieldKind::Number,true}});
        Add(TEXT("animation.section.add"), TEXT("umg"), {{TEXT("animation"),EFieldKind::String,true},{TEXT("bindingGuid"),EFieldKind::String,true},{TEXT("trackIndex"),EFieldKind::Number,true},{TEXT("startFrame"),EFieldKind::Number,true},{TEXT("endFrame"),EFieldKind::Number,true}});
        Add(TEXT("animation.section.remove"), TEXT("umg"), {{TEXT("animation"),EFieldKind::String,true},{TEXT("bindingGuid"),EFieldKind::String,true},{TEXT("trackIndex"),EFieldKind::Number,true},{TEXT("sectionIndex"),EFieldKind::Number,true}});
        Add(TEXT("animation.section.set"), TEXT("umg"), {{TEXT("animation"),EFieldKind::String,true},{TEXT("bindingGuid"),EFieldKind::String,true},{TEXT("trackIndex"),EFieldKind::Number,true},{TEXT("sectionIndex"),EFieldKind::Number,true},{TEXT("startFrame"),EFieldKind::Number,true},{TEXT("endFrame"),EFieldKind::Number,true},{TEXT("rowIndex"),EFieldKind::Number,false},{TEXT("active"),EFieldKind::Boolean,false},{TEXT("locked"),EFieldKind::Boolean,false}});
        const TArray<FFieldSpec> KeyIdentity = {{TEXT("animation"),EFieldKind::String,true},{TEXT("bindingGuid"),EFieldKind::String,true},{TEXT("trackIndex"),EFieldKind::Number,true},{TEXT("sectionIndex"),EFieldKind::Number,true},{TEXT("channelType"),EFieldKind::String,true},{TEXT("channelIndex"),EFieldKind::Number,true},{TEXT("frame"),EFieldKind::Number,true}};
        TArray<FFieldSpec> KeyAdd = KeyIdentity; KeyAdd.Add({TEXT("value"),EFieldKind::Any,true}); KeyAdd.Add({TEXT("interpolation"),EFieldKind::String,false}); Add(TEXT("animation.key.add"), TEXT("umg"), KeyAdd);
        TArray<FFieldSpec> KeyUpdate = KeyAdd; KeyUpdate.Add({TEXT("newFrame"),EFieldKind::Number,false}); Add(TEXT("animation.key.update"), TEXT("umg"), KeyUpdate);
        Add(TEXT("animation.key.remove"), TEXT("umg"), KeyIdentity);

        auto AnimGraphFields = [&GraphSelector](const TArray<FFieldSpec>& Specific)
        {
            TArray<FFieldSpec> Fields = GraphSelector;
            Fields.Append(Specific);
            return Fields;
        };
        const TArray<FFieldSpec> AnimNodeIdentity = {{TEXT("nodeGuid"),EFieldKind::String,true}};
        const TArray<FFieldSpec> AnimLinkIdentity = {{TEXT("fromNodeGuid"),EFieldKind::String,true},{TEXT("fromPinGuid"),EFieldKind::String,true},{TEXT("toNodeGuid"),EFieldKind::String,true},{TEXT("toPinGuid"),EFieldKind::String,true}};
        Add(TEXT("anim.skeleton.set"), TEXT("anim"), {{TEXT("skeletonPath"),EFieldKind::String,true}});
        Add(TEXT("anim.parent.set"), TEXT("anim"), {{TEXT("parentClassPath"),EFieldKind::String,true}});
        Add(TEXT("anim.node.spawn"), TEXT("anim"), AnimGraphFields({{TEXT("actionId"),EFieldKind::String,true},{TEXT("requestedNodeGuid"),EFieldKind::String,false},{TEXT("x"),EFieldKind::Number,false},{TEXT("y"),EFieldKind::Number,false}}));
        Add(TEXT("anim.node.delete"), TEXT("anim"), AnimGraphFields(AnimNodeIdentity));
        Add(TEXT("anim.node.move"), TEXT("anim"), AnimGraphFields({{TEXT("nodeGuid"),EFieldKind::String,true},{TEXT("x"),EFieldKind::Number,true},{TEXT("y"),EFieldKind::Number,true}}));
        Add(TEXT("anim.node.property.set"), TEXT("anim"), AnimGraphFields({{TEXT("nodeGuid"),EFieldKind::String,true},{TEXT("propertyPath"),EFieldKind::String,true},{TEXT("value"),EFieldKind::Any,true}}));
        Add(TEXT("anim.stateMachine.add"), TEXT("anim"), AnimGraphFields({{TEXT("actionId"),EFieldKind::String,true},{TEXT("requestedNodeGuid"),EFieldKind::String,false},{TEXT("name"),EFieldKind::String,false},{TEXT("x"),EFieldKind::Number,false},{TEXT("y"),EFieldKind::Number,false}}));
        Add(TEXT("anim.stateMachine.remove"), TEXT("anim"), AnimGraphFields(AnimNodeIdentity));
        Add(TEXT("anim.stateMachine.rename"), TEXT("anim"), AnimGraphFields({{TEXT("nodeGuid"),EFieldKind::String,true},{TEXT("name"),EFieldKind::String,true}}));
        Add(TEXT("anim.stateMachine.entry.set"), TEXT("anim"), AnimGraphFields({{TEXT("stateNodeGuid"),EFieldKind::String,true}}));
        const TArray<FFieldSpec> StateAddFields = {{TEXT("requestedNodeGuid"),EFieldKind::String,false},{TEXT("name"),EFieldKind::String,false},{TEXT("x"),EFieldKind::Number,false},{TEXT("y"),EFieldKind::Number,false}};
        Add(TEXT("anim.state.add"), TEXT("anim"), AnimGraphFields(StateAddFields));
        Add(TEXT("anim.state.remove"), TEXT("anim"), AnimGraphFields(AnimNodeIdentity));
        Add(TEXT("anim.state.rename"), TEXT("anim"), AnimGraphFields({{TEXT("nodeGuid"),EFieldKind::String,true},{TEXT("name"),EFieldKind::String,true}}));
        Add(TEXT("anim.conduit.add"), TEXT("anim"), AnimGraphFields(StateAddFields));
        Add(TEXT("anim.conduit.remove"), TEXT("anim"), AnimGraphFields(AnimNodeIdentity));
        Add(TEXT("anim.conduit.rename"), TEXT("anim"), AnimGraphFields({{TEXT("nodeGuid"),EFieldKind::String,true},{TEXT("name"),EFieldKind::String,true}}));
        Add(TEXT("anim.transition.add"), TEXT("anim"), AnimGraphFields({{TEXT("fromNodeGuid"),EFieldKind::String,true},{TEXT("toNodeGuid"),EFieldKind::String,true},{TEXT("requestedNodeGuid"),EFieldKind::String,false}}));
        Add(TEXT("anim.transition.remove"), TEXT("anim"), AnimGraphFields(AnimNodeIdentity));
        Add(TEXT("anim.transition.property.set"), TEXT("anim"), AnimGraphFields({{TEXT("nodeGuid"),EFieldKind::String,true},{TEXT("propertyPath"),EFieldKind::String,true},{TEXT("value"),EFieldKind::Any,true}}));
        Add(TEXT("anim.rule.node.spawn"), TEXT("anim"), AnimGraphFields({{TEXT("actionId"),EFieldKind::String,true},{TEXT("requestedNodeGuid"),EFieldKind::String,false},{TEXT("x"),EFieldKind::Number,false},{TEXT("y"),EFieldKind::Number,false}}));
        Add(TEXT("anim.rule.node.delete"), TEXT("anim"), AnimGraphFields(AnimNodeIdentity));
        Add(TEXT("anim.rule.node.property.set"), TEXT("anim"), AnimGraphFields({{TEXT("nodeGuid"),EFieldKind::String,true},{TEXT("propertyPath"),EFieldKind::String,true},{TEXT("value"),EFieldKind::Any,true}}));
        Add(TEXT("anim.rule.link.connect"), TEXT("anim"), AnimGraphFields(AnimLinkIdentity));
        Add(TEXT("anim.rule.link.disconnect"), TEXT("anim"), AnimGraphFields(AnimLinkIdentity));
        Add(TEXT("anim.poseLink.connect"), TEXT("anim"), AnimGraphFields(AnimLinkIdentity));
        Add(TEXT("anim.poseLink.disconnect"), TEXT("anim"), AnimGraphFields(AnimLinkIdentity));
        const TArray<FFieldSpec> AnimVariableFields = {{TEXT("name"),EFieldKind::String,true},{TEXT("type"),EFieldKind::Object,true},{TEXT("default"),EFieldKind::Any,false},{TEXT("category"),EFieldKind::String,false},{TEXT("tooltip"),EFieldKind::String,false},{TEXT("instanceEditable"),EFieldKind::Boolean,false},{TEXT("private"),EFieldKind::Boolean,false},{TEXT("transient"),EFieldKind::Boolean,false}};
        Add(TEXT("anim.variable.add"), TEXT("anim"), AnimVariableFields);
        TArray<FFieldSpec> AnimVariableUpdateFields = AnimVariableFields; AnimVariableUpdateFields.Add({TEXT("existingName"),EFieldKind::String,true}); Add(TEXT("anim.variable.update"), TEXT("anim"), AnimVariableUpdateFields);
        Add(TEXT("anim.variable.remove"), TEXT("anim"), {{TEXT("name"),EFieldKind::String,true}});
        Add(TEXT("anim.event.node.spawn"), TEXT("anim"), AnimGraphFields({{TEXT("actionId"),EFieldKind::String,true},{TEXT("requestedNodeGuid"),EFieldKind::String,false},{TEXT("x"),EFieldKind::Number,false},{TEXT("y"),EFieldKind::Number,false}}));
        Add(TEXT("anim.event.node.delete"), TEXT("anim"), AnimGraphFields(AnimNodeIdentity));
        Add(TEXT("anim.event.node.property.set"), TEXT("anim"), AnimGraphFields({{TEXT("nodeGuid"),EFieldKind::String,true},{TEXT("propertyPath"),EFieldKind::String,true},{TEXT("value"),EFieldKind::Any,true}}));
        Add(TEXT("anim.event.link.connect"), TEXT("anim"), AnimGraphFields(AnimLinkIdentity));
        Add(TEXT("anim.event.link.disconnect"), TEXT("anim"), AnimGraphFields(AnimLinkIdentity));

        SetFieldSchema(Definitions, TEXT("asset.create"), TEXT("kind"),
            StringEnumSchema({TEXT("blueprint"), TEXT("interface"), TEXT("functionLibrary"), TEXT("macroLibrary"), TEXT("struct"), TEXT("enum")}));
        SetFieldSchema(Definitions, TEXT("graph.add"), TEXT("kind"),
            StringEnumSchema({TEXT("event"), TEXT("construction"), TEXT("function"), TEXT("macro"), TEXT("interface"), TEXT("levelScript")}));
        SetFieldSchema(Definitions, TEXT("pin.default"), TEXT("valueKind"),
            StringEnumSchema({TEXT("string"), TEXT("object"), TEXT("text")}));
        SetFieldSchema(Definitions, TEXT("enum.value.rename"), TEXT("valueIndex"), IntegerSchema(0));
        SetFieldSchema(Definitions, TEXT("enum.value.remove"), TEXT("valueIndex"), IntegerSchema(0));
        SetFieldSchema(Definitions, TEXT("component.cloneRange"), TEXT("startIndex"), IntegerSchema());
        SetFieldSchema(Definitions, TEXT("component.cloneRange"), TEXT("endIndex"), IntegerSchema());
        SetFieldSchema(Definitions, TEXT("widget.add"), TEXT("childIndex"), IntegerSchema(0));
        SetFieldSchema(Definitions, TEXT("widget.reparent"), TEXT("childIndex"), IntegerSchema(0));
        SetFieldSchema(Definitions, TEXT("binding.set"), TEXT("kind"),
            StringEnumSchema({TEXT("function"), TEXT("property")}));
        SetFieldSchema(Definitions, TEXT("navigation.set"), TEXT("direction"),
            StringEnumSchema({TEXT("up"), TEXT("down"), TEXT("left"), TEXT("right"), TEXT("next"), TEXT("previous")}));
        SetFieldSchema(Definitions, TEXT("navigation.clear"), TEXT("direction"),
            StringEnumSchema({TEXT("up"), TEXT("down"), TEXT("left"), TEXT("right"), TEXT("next"), TEXT("previous")}));
        SetFieldSchema(Definitions, TEXT("navigation.set"), TEXT("rule"),
            StringEnumSchema({TEXT("escape"), TEXT("explicit"), TEXT("wrap"), TEXT("stop"), TEXT("custom"), TEXT("customboundary")}));
        SetFieldSchema(Definitions, TEXT("animation.binding.add"), TEXT("target"),
            StringEnumSchema({TEXT("widget"), TEXT("slot")}));
        SetFieldSchema(Definitions, TEXT("animation.key.add"), TEXT("interpolation"),
            StringEnumSchema({TEXT("constant"), TEXT("linear"), TEXT("cubic")}));
        SetFieldSchema(Definitions, TEXT("animation.key.update"), TEXT("interpolation"),
            StringEnumSchema({TEXT("constant"), TEXT("linear"), TEXT("cubic")}));

        const TArray<FString> AnimationIntegerOperations = {
            TEXT("animation.track.remove"), TEXT("animation.section.add"), TEXT("animation.section.remove"),
            TEXT("animation.section.set"), TEXT("animation.key.add"), TEXT("animation.key.update"),
            TEXT("animation.key.remove")};
        for (const FString& OperationName : AnimationIntegerOperations)
        {
            SetFieldSchema(Definitions, *OperationName, TEXT("trackIndex"), IntegerSchema(0));
            SetFieldSchema(Definitions, *OperationName, TEXT("sectionIndex"), IntegerSchema(0));
            SetFieldSchema(Definitions, *OperationName, TEXT("channelIndex"), IntegerSchema(0));
            SetFieldSchema(Definitions, *OperationName, TEXT("frame"), IntegerSchema());
        }
        SetFieldSchema(Definitions, TEXT("animation.add"), TEXT("tickResolution"), IntegerSchema(1));
        SetFieldSchema(Definitions, TEXT("animation.add"), TEXT("displayRate"), IntegerSchema(1));
        SetFieldSchema(Definitions, TEXT("animation.add"), TEXT("startFrame"), IntegerSchema());
        SetFieldSchema(Definitions, TEXT("animation.add"), TEXT("endFrame"), IntegerSchema());
        SetFieldSchema(Definitions, TEXT("animation.section.add"), TEXT("startFrame"), IntegerSchema());
        SetFieldSchema(Definitions, TEXT("animation.section.add"), TEXT("endFrame"), IntegerSchema());
        SetFieldSchema(Definitions, TEXT("animation.section.set"), TEXT("startFrame"), IntegerSchema());
        SetFieldSchema(Definitions, TEXT("animation.section.set"), TEXT("endFrame"), IntegerSchema());
        SetFieldSchema(Definitions, TEXT("animation.section.set"), TEXT("rowIndex"), IntegerSchema());
        SetFieldSchema(Definitions, TEXT("animation.key.update"), TEXT("newFrame"), IntegerSchema());

        TMap<FString, TSharedPtr<FJsonValue>> ComponentReferenceProperties;
        ComponentReferenceProperties.Add(TEXT("variableName"), MakeShared<FJsonValueObject>(NonEmptyStringSchema()));
        ComponentReferenceProperties.Add(TEXT("nodeGuid"), MakeShared<FJsonValueObject>(NonEmptyStringSchema()));
        ComponentReferenceProperties.Add(TEXT("ownerBlueprintPath"), MakeShared<FJsonValueObject>(NonEmptyStringSchema()));
        ComponentReferenceProperties.Add(TEXT("inherited"), MakeShared<FJsonValueObject>(TypeSchema(EFieldKind::Boolean)));
        TSharedRef<FJsonObject> ComponentReferenceSchema = ExactObjectSchema(ComponentReferenceProperties, {});
        TArray<TSharedPtr<FJsonValue>> ComponentIdentityChoices;
        TSharedRef<FJsonObject> ByName = TypeSchema(EFieldKind::Object);
        ByName->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("variableName"))});
        ComponentIdentityChoices.Add(MakeShared<FJsonValueObject>(ByName));
        TSharedRef<FJsonObject> ByGuid = TypeSchema(EFieldKind::Object);
        ByGuid->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("nodeGuid"))});
        ComponentIdentityChoices.Add(MakeShared<FJsonValueObject>(ByGuid));
        ComponentReferenceSchema->SetArrayField(TEXT("anyOf"), ComponentIdentityChoices);

        auto NumericObject = [](const TArray<FString>& Names) -> TSharedRef<FJsonObject>
        {
            TMap<FString, TSharedPtr<FJsonValue>> Properties;
            for (const FString& Name : Names) Properties.Add(Name, MakeShared<FJsonValueObject>(TypeSchema(EFieldKind::Number)));
            TSharedRef<FJsonObject> Schema = TypeSchema(EFieldKind::Object);
            TSharedRef<FJsonObject> PropertiesJson = MakeShared<FJsonObject>();
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties) PropertiesJson->SetField(Pair.Key, Pair.Value);
            Schema->SetObjectField(TEXT("properties"), PropertiesJson);
            TArray<TSharedPtr<FJsonValue>> Required;
            for (const FString& Name : Names) Required.Add(MakeShared<FJsonValueString>(Name));
            Schema->SetArrayField(TEXT("required"), Required);
            Schema->SetBoolField(TEXT("additionalProperties"), false);
            return Schema;
        };
        auto TextOrObject = [](const TSharedRef<FJsonObject>& ObjectSchema) -> TSharedRef<FJsonObject>
        {
            TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
            Schema->SetArrayField(TEXT("oneOf"), {MakeShared<FJsonValueObject>(NonEmptyStringSchema()), MakeShared<FJsonValueObject>(ObjectSchema)});
            return Schema;
        };
        TMap<FString, TSharedPtr<FJsonValue>> TransformProperties;
        const TSharedRef<FJsonObject> VectorSchema = NumericObject({TEXT("x"), TEXT("y"), TEXT("z")});
        const TSharedRef<FJsonObject> RotationSchema = NumericObject({TEXT("pitch"), TEXT("yaw"), TEXT("roll")});
        TransformProperties.Add(TEXT("location"), MakeShared<FJsonValueObject>(TextOrObject(VectorSchema)));
        TransformProperties.Add(TEXT("rotation"), MakeShared<FJsonValueObject>(TextOrObject(RotationSchema)));
        TransformProperties.Add(TEXT("scale"), MakeShared<FJsonValueObject>(TextOrObject(VectorSchema)));
        TSharedRef<FJsonObject> TransformSchema = ExactObjectSchema(TransformProperties,
            {TEXT("location"), TEXT("rotation"), TEXT("scale")});
        for (TPair<FString, FOperationDefinition>& Pair : Definitions)
        {
            const TSharedPtr<FJsonObject>* Properties = nullptr;
            if (!Pair.Value.Schema->TryGetObjectField(TEXT("properties"), Properties) || !Properties) continue;
            if ((*Properties)->HasField(TEXT("component"))) (*Properties)->SetObjectField(TEXT("component"), ComponentReferenceSchema);
            if ((*Properties)->HasField(TEXT("sourceComponent"))) (*Properties)->SetObjectField(TEXT("sourceComponent"), ComponentReferenceSchema);
            if ((*Properties)->HasField(TEXT("parent")) && Pair.Value.Domain == TEXT("component"))
                (*Properties)->SetObjectField(TEXT("parent"), ComponentReferenceSchema);
            if ((*Properties)->HasField(TEXT("transform"))) (*Properties)->SetObjectField(TEXT("transform"), TransformSchema);
            if (Pair.Value.Domain == TEXT("graph") && (*Properties)->HasField(TEXT("graphGuid")))
            {
                TSharedRef<FJsonObject> GuidChoice = TypeSchema(EFieldKind::Object);
                GuidChoice->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("graphGuid"))});
                TSharedRef<FJsonObject> NameChoice = TypeSchema(EFieldKind::Object);
                NameChoice->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("graphName"))});
                Pair.Value.Schema->SetArrayField(TEXT("oneOf"), {
                    MakeShared<FJsonValueObject>(GuidChoice), MakeShared<FJsonValueObject>(NameChoice)});
            }
        }

        TMap<FString, TSharedPtr<FJsonValue>> MemberReferenceProperties;
        MemberReferenceProperties.Add(TEXT("parentPath"), MakeShared<FJsonValueObject>(NonEmptyStringSchema()));
        MemberReferenceProperties.Add(TEXT("name"), MakeShared<FJsonValueObject>(NonEmptyStringSchema()));
        MemberReferenceProperties.Add(TEXT("guid"), MakeShared<FJsonValueObject>(NonEmptyStringSchema()));
        TSharedRef<FJsonObject> MemberReferenceSchema = ExactObjectSchema(MemberReferenceProperties, {TEXT("name")});
        TMap<FString, TSharedPtr<FJsonValue>> PinTypeProperties;
        PinTypeProperties.Add(TEXT("category"), MakeShared<FJsonValueObject>(StringEnumSchema({TEXT("exec"), TEXT("boolean"), TEXT("byte"), TEXT("class"), TEXT("int"), TEXT("int64"), TEXT("float"), TEXT("name"), TEXT("delegate"), TEXT("mcdelegate"), TEXT("object"), TEXT("interface"), TEXT("string"), TEXT("text"), TEXT("struct"), TEXT("wildcard"), TEXT("softobject"), TEXT("softclass")})));
        PinTypeProperties.Add(TEXT("subCategory"), MakeShared<FJsonValueObject>(TypeSchema(EFieldKind::String)));
        PinTypeProperties.Add(TEXT("subCategoryObjectPath"), MakeShared<FJsonValueObject>(NonEmptyStringSchema()));
        PinTypeProperties.Add(TEXT("memberReference"), MakeShared<FJsonValueObject>(MemberReferenceSchema));
        PinTypeProperties.Add(TEXT("container"), MakeShared<FJsonValueObject>(StringEnumSchema({TEXT("none"), TEXT("array"), TEXT("set"), TEXT("map")})));
        PinTypeProperties.Add(TEXT("isReference"), MakeShared<FJsonValueObject>(TypeSchema(EFieldKind::Boolean)));
        PinTypeProperties.Add(TEXT("isConst"), MakeShared<FJsonValueObject>(TypeSchema(EFieldKind::Boolean)));
        PinTypeProperties.Add(TEXT("isWeakPointer"), MakeShared<FJsonValueObject>(TypeSchema(EFieldKind::Boolean)));
        PinTypeProperties.Add(TEXT("isUObjectWrapper"), MakeShared<FJsonValueObject>(TypeSchema(EFieldKind::Boolean)));
        TSharedRef<FJsonObject> TerminalTypeSchema = ExactObjectSchema(PinTypeProperties, {TEXT("category")});
        PinTypeProperties.Add(TEXT("valueType"), MakeShared<FJsonValueObject>(TerminalTypeSchema));
        TSharedRef<FJsonObject> PinTypeSchema = ExactObjectSchema(PinTypeProperties, {TEXT("category")});
        for (TPair<FString, FOperationDefinition>& Pair : Definitions)
        {
            if (Pair.Value.Domain != TEXT("type") && Pair.Value.Domain != TEXT("graph")) continue;
            const TSharedPtr<FJsonObject>* Properties = nullptr;
            if (Pair.Value.Schema->TryGetObjectField(TEXT("properties"), Properties) && Properties
                && (*Properties)->HasField(TEXT("type"))) (*Properties)->SetObjectField(TEXT("type"), PinTypeSchema);
        }

        TSharedRef<FJsonObject> MetadataSchema = TypeSchema(EFieldKind::Object);
        MetadataSchema->SetObjectField(TEXT("additionalProperties"), TypeSchema(EFieldKind::String));
        SetFieldSchema(Definitions, TEXT("variable.add"), TEXT("metadata"), MetadataSchema);
        SetFieldSchema(Definitions, TEXT("variable.update"), TEXT("metadata"), MetadataSchema);

        TMap<FString, TSharedPtr<FJsonValue>> ParameterProperties;
        ParameterProperties.Add(TEXT("name"), MakeShared<FJsonValueObject>(NonEmptyStringSchema()));
        ParameterProperties.Add(TEXT("type"), MakeShared<FJsonValueObject>(PinTypeSchema));
        ParameterProperties.Add(TEXT("defaultValue"), MakeShared<FJsonValueObject>(TypeSchema(EFieldKind::String)));
        TSharedRef<FJsonObject> ParameterSchema = ExactObjectSchema(ParameterProperties, {TEXT("name"), TEXT("type")});
        TSharedRef<FJsonObject> ParameterArraySchema = TypeSchema(EFieldKind::Array);
        ParameterArraySchema->SetObjectField(TEXT("items"), ParameterSchema);
        SetFieldSchema(Definitions, TEXT("signature.set"), TEXT("inputs"), ParameterArraySchema);
        SetFieldSchema(Definitions, TEXT("signature.set"), TEXT("outputs"), ParameterArraySchema);

        TSharedRef<FJsonObject> GuidArraySchema = TypeSchema(EFieldKind::Array);
        GuidArraySchema->SetNumberField(TEXT("minItems"), 1);
        GuidArraySchema->SetObjectField(TEXT("items"), NonEmptyStringSchema());
        SetFieldSchema(Definitions, TEXT("node.copy"), TEXT("nodeGuids"), GuidArraySchema);
        SetFieldSchema(Definitions, TEXT("graph.layout"), TEXT("nodeGuids"), GuidArraySchema);

        const TSet<FString> NonEmptyFields = {
            TEXT("assetPath"), TEXT("packagePath"), TEXT("destinationPath"), TEXT("parentClassPath"),
            TEXT("interfaceClassPath"), TEXT("classPath"), TEXT("variableName"), TEXT("newName"),
            TEXT("propertyPath"), TEXT("name"), TEXT("existingName"), TEXT("displayName"), TEXT("fieldGuid"),
            TEXT("graphGuid"), TEXT("graphName"), TEXT("signatureNodeGuid"), TEXT("actionId"), TEXT("customEventName"),
            TEXT("nodeGuid"), TEXT("pinGuid"), TEXT("aNodeGuid"), TEXT("aPinGuid"), TEXT("bNodeGuid"), TEXT("bPinGuid"),
            TEXT("fromNodeGuid"), TEXT("fromPinGuid"), TEXT("toNodeGuid"), TEXT("toPinGuid"),
            TEXT("stateNodeGuid"), TEXT("requestedNodeGuid"), TEXT("repNotifyFunction"), TEXT("widget"),
            TEXT("animation"), TEXT("bindingGuid"), TEXT("channelType"), TEXT("skeletonPath"), TEXT("event"),
            TEXT("property"), TEXT("source")
        };
        for (TPair<FString, FOperationDefinition>& Pair : Definitions)
        {
            const TSharedPtr<FJsonObject>* Properties = nullptr;
            if (!Pair.Value.Schema->TryGetObjectField(TEXT("properties"), Properties) || !Properties) continue;
            for (const FString& Field : NonEmptyFields)
            {
                const TSharedPtr<FJsonObject>* Existing = nullptr;
                if ((*Properties)->TryGetObjectField(Field, Existing) && Existing)
                {
                    FString Type;
                    if ((*Existing)->TryGetStringField(TEXT("type"), Type) && Type == TEXT("string")
                        && !(*Existing)->HasField(TEXT("enum")) && !(*Existing)->HasField(TEXT("const")))
                        (*Existing)->SetNumberField(TEXT("minLength"), 1);
                }
            }
        }
#undef CODEX_ADD
    }

    const FOperationDefinition* FOperationRegistry::Find(const FString& Name) const
    {
        return Definitions.Find(Name);
    }

    TSharedRef<FJsonObject> FOperationRegistry::GetCapabilities(const FString& Domain,
        const TArray<FString>& OperationNames, FProtocolError& OutError) const
    {
        TArray<FString> Names; Definitions.GetKeys(Names); Names.Sort(); TArray<TSharedPtr<FJsonValue>> Items;
        for (const FString& Name : Names)
        {
            const FOperationDefinition& Definition = Definitions.FindChecked(Name);
            if (!Domain.IsEmpty() && Definition.Domain != Domain) continue;
            if (OperationNames.Num() && !OperationNames.Contains(Name)) continue;
            TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("operation"), Name); Item->SetStringField(TEXT("domain"), Definition.Domain); Item->SetStringField(TEXT("description"), Definition.Description); Item->SetObjectField(TEXT("schema"), Definition.Schema); Item->SetObjectField(TEXT("example"), Definition.Example); Items.Add(MakeShared<FJsonValueObject>(Item));
        }
        for (const FString& Requested : OperationNames) if (!Definitions.Contains(Requested)) { OutError = OperationError(EErrorCode::UnknownOperation, FString::Printf(TEXT("Unknown operation '%s'."), *Requested), TEXT("FOperationRegistry::GetCapabilities")); return MakeShared<FJsonObject>(); }
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>(); Result->SetStringField(TEXT("schemaSource"), TEXT("CodexUnrealBlueprintOperationRegistry")); Result->SetArrayField(TEXT("operations"), Items); return Result;
    }

    TSharedRef<FJsonObject> FOperationRegistry::Search(const FString& Query, const FString& Domain, int32 Offset, int32 Limit) const
    {
        TArray<FString> Names; Definitions.GetKeys(Names); Names.Sort(); TArray<FString> Matches;
        for (const FString& Name : Names) if ((Domain.IsEmpty() || Domain == TEXT("operation") || Definitions.FindChecked(Name).Domain == Domain) && Name.Contains(Query, ESearchCase::IgnoreCase)) Matches.Add(Name);
        TArray<TSharedPtr<FJsonValue>> Items; const int32 End = FMath::Min(Matches.Num(), Offset + Limit); for (int32 Index = Offset; Index < End; ++Index) { TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("operation"), Matches[Index]); Item->SetStringField(TEXT("domain"), Definitions.FindChecked(Matches[Index]).Domain); Items.Add(MakeShared<FJsonValueObject>(Item)); }
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>(); Result->SetNumberField(TEXT("total"), Matches.Num()); Result->SetArrayField(TEXT("items"), Items); if (End < Matches.Num()) Result->SetStringField(TEXT("nextCursor"), FString::FromInt(End)); return Result;
    }

    bool FOperationRegistry::Validate(const TArray<TSharedRef<FJsonObject>>& Operations,
        FPreflightRequest& OutPreflight, TSharedRef<FJsonObject>& OutResult, FProtocolError& OutError) const
    {
        OutPreflight = FPreflightRequest(); TArray<TSharedPtr<FJsonValue>> Validated;
        for (int32 Index = 0; Index < Operations.Num(); ++Index)
        {
            FString Name; if (!Operations[Index]->TryGetStringField(TEXT("operation"), Name) || Name.IsEmpty()) { OutError = OperationError(EErrorCode::InvalidArgument, TEXT("Every operation requires a non-empty operation discriminator."), TEXT("FOperationRegistry::Validate"), Index); return false; }
            const FOperationDefinition* Definition = Definitions.Find(Name); if (!Definition) { OutError = OperationError(EErrorCode::UnknownOperation, FString::Printf(TEXT("Unknown operation '%s'."), *Name), TEXT("FOperationRegistry::Validate"), Index); return false; }
            FString SchemaMessage;
            if (!ValidateSchemaValue(MakeShared<FJsonValueObject>(Operations[Index]), *Definition->Schema,
                FString::Printf(TEXT("operations[%d]"), Index), SchemaMessage))
            {
                OutError = OperationError(EErrorCode::TypeMismatch, SchemaMessage,
                    TEXT("FOperationRegistry::Validate"), Index);
                return false;
            }
            if (Name == TEXT("component.add"))
            {
                FString AssetPath, ClassPath, VariableName;
                Operations[Index]->TryGetStringField(TEXT("assetPath"), AssetPath);
                Operations[Index]->TryGetStringField(TEXT("classPath"), ClassPath);
                Operations[Index]->TryGetStringField(TEXT("variableName"), VariableName);
                UBlueprint* Blueprint = Cast<UBlueprint>(LoadExactObject(AssetPath));
                UClass* ComponentClass = LoadExactClass(ClassPath);
                if (Blueprint && ComponentClass && ComponentClass->IsChildOf(UActorComponent::StaticClass())
                    && Blueprint->SimpleConstructionScript && Blueprint->SimpleConstructionScript->FindSCSNode(FName(*VariableName)))
                {
                    OutError = OperationError(EErrorCode::ValidationFailed, FString::Printf(TEXT("Component '%s' already exists."), *VariableName),
                        TEXT("USimpleConstructionScript::FindSCSNode"), Index, AssetPath); return false;
                }
                const TSharedPtr<FJsonObject>* TransformJson = nullptr; FTransform Transform;
                if (Operations[Index]->TryGetObjectField(TEXT("transform"), TransformJson) && !ParseTransform(*TransformJson, Transform))
                {
                    OutError = OperationError(EErrorCode::TypeMismatch, TEXT("Invalid transform at operations[].transform; use location/scale {x,y,z} and rotation {pitch,yaw,roll}, or Unreal text."),
                        TEXT("ParseTransform"), Index, AssetPath); return false;
                }
                const TSharedPtr<FJsonObject>* InitialProperties = nullptr;
                if (Blueprint && ComponentClass && ComponentClass->IsChildOf(UActorComponent::StaticClass())
                    && Operations[Index]->TryGetObjectField(TEXT("initialProperties"), InitialProperties) && InitialProperties)
                {
                    UActorComponent* Scratch = DuplicateObject<UActorComponent>(Cast<UActorComponent>(ComponentClass->GetDefaultObject()), GetTransientPackage());
                    for (const TPair<FString, TSharedPtr<FJsonValue>>& Property : (*InitialProperties)->Values)
                    {
                        FBlueprintOperationError Error;
                        if (!Scratch || !FBlueprintTypeSystem::SetPropertyValue(Scratch, Property.Key, Property.Value, Error, AssetPath, Index))
                        {
                            OutError = OperationError(EErrorCode::ValidationFailed, Error.Message.IsEmpty() ? TEXT("initialProperties contains an invalid property or value.") : Error.Message,
                                Error.UECallsite.IsEmpty() ? TEXT("FBlueprintTypeSystem::SetPropertyValue") : Error.UECallsite, Index, AssetPath); return false;
                        }
                    }
                }
            }
            if (Name == TEXT("component.cloneRange"))
            {
                FString AssetPath, Pattern; double Start = 0.0, End = -1.0;
                const TSharedPtr<FJsonObject>* SourceJson = nullptr; FComponentReference Source;
                const TSharedPtr<FJsonObject>* Overrides = nullptr;
                Operations[Index]->TryGetStringField(TEXT("assetPath"), AssetPath);
                Operations[Index]->TryGetStringField(TEXT("targetPattern"), Pattern);
                Operations[Index]->TryGetNumberField(TEXT("startIndex"), Start);
                Operations[Index]->TryGetNumberField(TEXT("endIndex"), End);
                Operations[Index]->TryGetObjectField(TEXT("sourceComponent"), SourceJson);
                Operations[Index]->TryGetObjectField(TEXT("propertyOverrides"), Overrides);
                FBlueprintOperationResult Semantic = FBlueprintComponentOperations::ValidateCloneRange(
                    Cast<UBlueprint>(LoadExactObject(AssetPath)), SourceJson && ParseComponentReference(*SourceJson, Source) ? Source : FComponentReference(),
                    Pattern, static_cast<int32>(Start), static_cast<int32>(End), Overrides ? *Overrides : nullptr, Index);
                if (!Semantic.bSuccess)
                {
                    const FBlueprintOperationError Error = Semantic.Error.IsSet() ? Semantic.Error.GetValue() : FBlueprintOperationError();
                    OutError = OperationError(EErrorCode::ValidationFailed, Error.Message.IsEmpty() ? TEXT("component.cloneRange semantic validation failed.") : Error.Message,
                        Error.UECallsite.IsEmpty() ? TEXT("FBlueprintComponentOperations::ValidateCloneRange") : Error.UECallsite, Index, AssetPath);
                    return false;
                }
            }
            const TSharedRef<FRegistryWriteOperation> WriteOperation = MakeShared<FRegistryWriteOperation>(Operations[Index], Index);
            WriteOperation->GatherPreflight(OutPreflight);
            TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>(); Item->SetNumberField(TEXT("operationIndex"), Index); Item->SetStringField(TEXT("operation"), Name); Item->SetStringField(TEXT("domain"), Definition->Domain); Validated.Add(MakeShared<FJsonValueObject>(Item));
        }
        OutResult = MakeShared<FJsonObject>(); OutResult->SetBoolField(TEXT("valid"), true); OutResult->SetArrayField(TEXT("operations"), Validated); TArray<TSharedPtr<FJsonValue>> Packages; for (const FString& Package : OutPreflight.TargetPackageNames) Packages.Add(MakeShared<FJsonValueString>(Package)); OutResult->SetArrayField(TEXT("impactPackages"), Packages); return true;
    }

    bool FOperationRegistry::BuildWriteRequest(const FString& RequestId,
        const TArray<TSharedRef<FJsonObject>>& Operations, const TMap<FString, FString>& ExpectedStateHashes,
        FWritePipelineRequest& OutRequest, FProtocolError& OutError) const
    {
        if (RequestId.TrimStartAndEnd().IsEmpty()) { OutError = OperationError(EErrorCode::RequestIdRequired, TEXT("blueprint.apply requires a non-empty requestId."), TEXT("FOperationRegistry::BuildWriteRequest")); return false; }
        TSharedRef<FJsonObject> Ignored = MakeShared<FJsonObject>(); FPreflightRequest ValidatedPreflight;
        if (!Validate(Operations, ValidatedPreflight, Ignored, OutError)) return false;
        OutRequest = FWritePipelineRequest(); OutRequest.RequestId = RequestId; OutRequest.TransactionDescription = FString::Printf(TEXT("Codex Blueprint apply %s"), *RequestId); OutRequest.Preflight.ExpectedStructureHashes = ExpectedStateHashes;
        for (int32 Index = 0; Index < Operations.Num(); ++Index) OutRequest.Operations.Add(MakeShared<FRegistryWriteOperation>(Operations[Index], Index));
        return true;
    }
}
