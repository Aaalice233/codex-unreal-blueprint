#include "CodexUnrealAssetInspection.h"

#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimBlueprint.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "CodexUnrealBlueprintInspection.h"
#include "CodexUnrealBlueprintTypeSystem.h"
#include "Engine/Blueprint.h"
#include "Engine/UserDefinedEnum.h"
#include "Engine/UserDefinedStruct.h"
#include "Engine/World.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraParameterStore.h"
#include "NiagaraSystem.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"

namespace CodexUnrealBlueprint
{
    namespace
    {
        const TSet<FString> KnownFacets = {
            TEXT("support"), TEXT("generic"), TEXT("properties"), TEXT("dependencies"),
            TEXT("referencers"), TEXT("specialized")
        };

        bool WantsFacet(const TArray<FString>& Facets, const FString& Facet)
        {
            return Facets.Num() == 0 || Facets.Contains(Facet);
        }

        TSharedRef<FJsonObject> PageArray(const TArray<TSharedPtr<FJsonValue>>& All, const int32 Offset,
            const int32 Limit)
        {
            TArray<TSharedPtr<FJsonValue>> Items;
            const int32 End = FMath::Min(All.Num(), Offset + Limit);
            for (int32 Index = FMath::Min(Offset, All.Num()); Index < End; ++Index) Items.Add(All[Index]);
            TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetNumberField(TEXT("total"), All.Num());
            Result->SetArrayField(TEXT("items"), Items);
            if (End < All.Num()) Result->SetStringField(TEXT("nextCursor"), FString::FromInt(End));
            return Result;
        }

        FString CanonicalJson(const TSharedRef<FJsonObject>& Object)
        {
            FString Json;
            const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
            FJsonSerializer::Serialize(Object, Writer);
            return Json;
        }

        FString Sha1(const TSharedRef<FJsonObject>& Object)
        {
            const FString Json = CanonicalJson(Object);
            FTCHARToUTF8 Utf8(*Json);
            uint8 Digest[FSHA1::DigestSize];
            FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Digest);
            return BytesToHex(Digest, FSHA1::DigestSize).ToLower();
        }

        FString PackageNameForPath(const FString& AssetPath)
        {
            if (AssetPath.Contains(TEXT("."))) return FPackageName::ObjectPathToPackageName(AssetPath);
            return AssetPath;
        }

        UObject* LoadAsset(const FString& AssetPath, FProtocolError& OutError)
        {
            UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath, nullptr, LOAD_NoWarn);
            if (!Asset && FPackageName::IsValidLongPackageName(AssetPath))
            {
                const FString ObjectPath = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
                Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath, nullptr, LOAD_NoWarn);
            }
            if (!Asset)
            {
                OutError = FProtocolError::Make(EErrorCode::AssetNotFound,
                    FString::Printf(TEXT("Asset '%s' was not found."), *AssetPath),
                    TEXT("FUnrealAssetInspection::LoadAsset"));
                OutError.AssetPath = AssetPath;
            }
            return Asset;
        }

        bool IsEditableAsset(UObject* Asset)
        {
            return Asset->IsA<UBlueprint>() || Asset->IsA<UWorld>() || Asset->IsA<UUserDefinedStruct>()
                || Asset->IsA<UUserDefinedEnum>();
        }

        TArray<FString> Specializations(UObject* Asset)
        {
            TArray<FString> Values;
            if (Asset->IsA<UBlueprint>() || Asset->IsA<UWorld>() || Asset->IsA<UUserDefinedStruct>()
                || Asset->IsA<UUserDefinedEnum>()) Values.Add(TEXT("blueprint"));
            if (Asset->IsA<UWidgetBlueprint>()) Values.Add(TEXT("umg"));
            if (Asset->IsA<UAnimBlueprint>()) Values.Add(TEXT("animBlueprint"));
            if (Asset->IsA<UAnimMontage>()) Values.Add(TEXT("animMontage"));
            if (Asset->IsA<UMaterialInterface>()) Values.Add(TEXT("material"));
            if (Asset->IsA<UNiagaraSystem>()) Values.Add(TEXT("niagaraSystem"));
            return Values;
        }

        TSharedRef<FJsonObject> SupportSnapshot(UObject* Asset)
        {
            const TArray<FString> SpecializationNames = Specializations(Asset);
            TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("generic"), true);
            Result->SetBoolField(TEXT("specialized"), SpecializationNames.Num() > 0);
            Result->SetBoolField(TEXT("editable"), IsEditableAsset(Asset));
            TArray<TSharedPtr<FJsonValue>> Names;
            for (const FString& Name : SpecializationNames) Names.Add(MakeShared<FJsonValueString>(Name));
            Result->SetArrayField(TEXT("specializations"), Names);
            Result->SetStringField(TEXT("highestLayer"), IsEditableAsset(Asset)
                ? TEXT("editable") : SpecializationNames.Num() > 0 ? TEXT("specialized") : TEXT("generic"));
            return Result;
        }

        TSharedRef<FJsonObject> GenericSnapshot(UObject* Asset)
        {
            TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("assetPath"), Asset->GetPathName());
            Result->SetStringField(TEXT("assetName"), Asset->GetName());
            Result->SetStringField(TEXT("classPath"), Asset->GetClass()->GetPathName());
            Result->SetStringField(TEXT("packageName"), Asset->GetOutermost()->GetName());
            Result->SetStringField(TEXT("outerPath"), Asset->GetOuter() ? Asset->GetOuter()->GetPathName() : FString());
            Result->SetBoolField(TEXT("packageDirty"), Asset->GetOutermost()->IsDirty());
            Result->SetBoolField(TEXT("isAsset"), Asset->IsAsset());
            return Result;
        }

        bool PropertiesSnapshot(UObject* Asset, const TArray<FString>& PropertyPaths, const int32 Offset,
            const int32 Limit, TSharedRef<FJsonObject>& OutResult, FProtocolError& OutError)
        {
            TArray<TSharedPtr<FJsonValue>> Descriptors;
            for (TFieldIterator<FProperty> It(Asset->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
            {
                TSharedRef<FJsonObject> Descriptor = MakeShared<FJsonObject>();
                Descriptor->SetStringField(TEXT("name"), It->GetName());
                Descriptor->SetStringField(TEXT("cppType"), It->GetCPPType());
                Descriptor->SetStringField(TEXT("owner"), It->GetOwnerStruct() ? It->GetOwnerStruct()->GetPathName() : FString());
                Descriptors.Add(MakeShared<FJsonValueObject>(Descriptor));
            }
            Descriptors.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
            {
                return A->AsObject()->GetStringField(TEXT("name")) < B->AsObject()->GetStringField(TEXT("name"));
            });
            OutResult->SetObjectField(TEXT("descriptors"), PageArray(Descriptors, Offset, Limit));

            TSharedRef<FJsonObject> Values = MakeShared<FJsonObject>();
            for (const FString& PropertyPath : PropertyPaths)
            {
                FBlueprintOperationError Error;
                const TSharedPtr<FJsonValue> Value = FBlueprintTypeSystem::GetPropertyValue(
                    Asset, PropertyPath, Error, Asset->GetPathName());
                if (!Value.IsValid())
                {
                    OutError = FProtocolError::Make(EErrorCode::ValidationFailed, Error.Message,
                        Error.UECallsite.IsEmpty() ? TEXT("FBlueprintTypeSystem::GetPropertyValue") : Error.UECallsite);
                    OutError.AssetPath = Asset->GetPathName();
                    return false;
                }
                Values->SetField(PropertyPath, Value);
            }
            OutResult->SetObjectField(TEXT("values"), Values);
            return true;
        }

        TSharedRef<FJsonObject> PackageReferences(const FName PackageName, const bool bReferencers,
            const int32 Offset, const int32 Limit)
        {
            IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
            TArray<FName> Packages;
            if (bReferencers)
                Registry.GetReferencers(PackageName, Packages, UE::AssetRegistry::EDependencyCategory::Package);
            else
                Registry.GetDependencies(PackageName, Packages, UE::AssetRegistry::EDependencyCategory::Package);
            Packages.Sort(FNameLexicalLess());
            TArray<TSharedPtr<FJsonValue>> Items;
            for (const FName& Package : Packages)
            {
                TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
                Item->SetStringField(TEXT("packageName"), Package.ToString());
                TArray<FAssetData> Assets;
                Registry.GetAssetsByPackageName(Package, Assets, true);
                TArray<TSharedPtr<FJsonValue>> AssetValues;
                for (const FAssetData& AssetData : Assets)
                {
                    TSharedRef<FJsonObject> AssetValue = MakeShared<FJsonObject>();
                    AssetValue->SetStringField(TEXT("assetPath"), AssetData.ObjectPath.ToString());
                    AssetValue->SetStringField(TEXT("className"), AssetData.AssetClass.ToString());
                    AssetValues.Add(MakeShared<FJsonValueObject>(AssetValue));
                }
                Item->SetArrayField(TEXT("assets"), AssetValues);
                Items.Add(MakeShared<FJsonValueObject>(Item));
            }
            return PageArray(Items, Offset, Limit);
        }

        TSharedRef<FJsonObject> MaterialSnapshot(UMaterialInterface* Material)
        {
            TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("assetPath"), Material->GetPathName());
            Result->SetStringField(TEXT("baseMaterial"), Material->GetMaterial() ? Material->GetMaterial()->GetPathName() : FString());
            if (UMaterialInstance* Instance = Cast<UMaterialInstance>(Material))
                Result->SetStringField(TEXT("parent"), Instance->Parent ? Instance->Parent->GetPathName() : FString());

            TArray<FMaterialParameterInfo> Infos;
            TArray<FGuid> Ids;
            TArray<TSharedPtr<FJsonValue>> Scalars;
            Material->GetAllScalarParameterInfo(Infos, Ids);
            for (const FMaterialParameterInfo& Info : Infos)
            {
                float Value = 0.0f;
                TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
                Json->SetStringField(TEXT("name"), Info.Name.ToString());
                Json->SetNumberField(TEXT("association"), static_cast<int32>(Info.Association));
                Json->SetNumberField(TEXT("index"), Info.Index);
                if (Material->GetScalarParameterValue(FHashedMaterialParameterInfo(Info), Value)) Json->SetNumberField(TEXT("value"), Value);
                Scalars.Add(MakeShared<FJsonValueObject>(Json));
            }
            Result->SetArrayField(TEXT("scalarParameters"), Scalars);

            Infos.Reset(); Ids.Reset();
            TArray<TSharedPtr<FJsonValue>> Vectors;
            Material->GetAllVectorParameterInfo(Infos, Ids);
            for (const FMaterialParameterInfo& Info : Infos)
            {
                FLinearColor Value;
                TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
                Json->SetStringField(TEXT("name"), Info.Name.ToString());
                if (Material->GetVectorParameterValue(FHashedMaterialParameterInfo(Info), Value))
                {
                    Json->SetNumberField(TEXT("r"), Value.R); Json->SetNumberField(TEXT("g"), Value.G);
                    Json->SetNumberField(TEXT("b"), Value.B); Json->SetNumberField(TEXT("a"), Value.A);
                }
                Vectors.Add(MakeShared<FJsonValueObject>(Json));
            }
            Result->SetArrayField(TEXT("vectorParameters"), Vectors);

            Infos.Reset(); Ids.Reset();
            TArray<TSharedPtr<FJsonValue>> Textures;
            Material->GetAllTextureParameterInfo(Infos, Ids);
            for (const FMaterialParameterInfo& Info : Infos)
            {
                UTexture* Value = nullptr;
                TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
                Json->SetStringField(TEXT("name"), Info.Name.ToString());
                if (Material->GetTextureParameterValue(FHashedMaterialParameterInfo(Info), Value))
                    Json->SetStringField(TEXT("value"), Value ? Value->GetPathName() : FString());
                Textures.Add(MakeShared<FJsonValueObject>(Json));
            }
            Result->SetArrayField(TEXT("textureParameters"), Textures);

            Infos.Reset(); Ids.Reset();
            TArray<TSharedPtr<FJsonValue>> StaticSwitches;
            Material->GetAllStaticSwitchParameterInfo(Infos, Ids);
            for (const FMaterialParameterInfo& Info : Infos)
            {
                bool Value = false; FGuid ExpressionGuid;
                TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
                Json->SetStringField(TEXT("name"), Info.Name.ToString());
                if (Material->GetStaticSwitchParameterValue(FHashedMaterialParameterInfo(Info), Value, ExpressionGuid))
                {
                    Json->SetBoolField(TEXT("value"), Value);
                    Json->SetStringField(TEXT("expressionGuid"), ExpressionGuid.ToString(EGuidFormats::DigitsWithHyphens));
                }
                StaticSwitches.Add(MakeShared<FJsonValueObject>(Json));
            }
            Result->SetArrayField(TEXT("staticSwitchParameters"), StaticSwitches);

            if (UMaterial* Base = Cast<UMaterial>(Material))
            {
                TArray<TSharedPtr<FJsonValue>> Expressions;
                for (UMaterialExpression* Expression : Base->Expressions)
                {
                    if (!Expression) continue;
                    TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
                    Json->SetStringField(TEXT("name"), Expression->GetName());
                    Json->SetStringField(TEXT("classPath"), Expression->GetClass()->GetPathName());
                    Json->SetNumberField(TEXT("x"), Expression->MaterialExpressionEditorX);
                    Json->SetNumberField(TEXT("y"), Expression->MaterialExpressionEditorY);
                    Json->SetStringField(TEXT("description"), Expression->Desc);
                    TArray<UMaterialExpression*> Inputs; Expression->GetAllInputExpressions(Inputs);
                    TArray<TSharedPtr<FJsonValue>> InputValues;
                    for (UMaterialExpression* Input : Inputs)
                        if (Input) InputValues.Add(MakeShared<FJsonValueString>(Input->GetName()));
                    Json->SetArrayField(TEXT("inputs"), InputValues);
                    Expressions.Add(MakeShared<FJsonValueObject>(Json));
                }
                Result->SetArrayField(TEXT("expressions"), Expressions);
            }
            return Result;
        }

        TSharedRef<FJsonObject> MontageSnapshot(UAnimMontage* Montage)
        {
            TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("assetPath"), Montage->GetPathName());
            Result->SetStringField(TEXT("skeleton"), Montage->GetSkeleton() ? Montage->GetSkeleton()->GetPathName() : FString());
            Result->SetNumberField(TEXT("sequenceLength"), Montage->SequenceLength);
            Result->SetNumberField(TEXT("rateScale"), Montage->RateScale);
            Result->SetNumberField(TEXT("blendInTime"), Montage->BlendIn.GetBlendTime());
            Result->SetNumberField(TEXT("blendOutTime"), Montage->BlendOut.GetBlendTime());
            Result->SetNumberField(TEXT("blendOutTriggerTime"), Montage->BlendOutTriggerTime);
            Result->SetBoolField(TEXT("enableAutoBlendOut"), Montage->bEnableAutoBlendOut);

            TArray<TSharedPtr<FJsonValue>> Sections;
            for (const FCompositeSection& Section : Montage->CompositeSections)
            {
                TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
                Json->SetStringField(TEXT("name"), Section.SectionName.ToString());
                Json->SetStringField(TEXT("nextSection"), Section.NextSectionName.ToString());
                Json->SetNumberField(TEXT("time"), Section.GetTime());
                Sections.Add(MakeShared<FJsonValueObject>(Json));
            }
            Result->SetArrayField(TEXT("sections"), Sections);

            TArray<TSharedPtr<FJsonValue>> Slots;
            for (const FSlotAnimationTrack& Slot : Montage->SlotAnimTracks)
            {
                TSharedRef<FJsonObject> SlotJson = MakeShared<FJsonObject>();
                SlotJson->SetStringField(TEXT("name"), Slot.SlotName.ToString());
                TArray<TSharedPtr<FJsonValue>> Segments;
                for (const FAnimSegment& Segment : Slot.AnimTrack.AnimSegments)
                {
                    TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
                    Json->SetStringField(TEXT("animation"), Segment.AnimReference ? Segment.AnimReference->GetPathName() : FString());
                    Json->SetNumberField(TEXT("startPosition"), Segment.StartPos);
                    Json->SetNumberField(TEXT("animationStartTime"), Segment.AnimStartTime);
                    Json->SetNumberField(TEXT("animationEndTime"), Segment.AnimEndTime);
                    Json->SetNumberField(TEXT("playRate"), Segment.AnimPlayRate);
                    Json->SetNumberField(TEXT("loopCount"), Segment.LoopingCount);
                    Segments.Add(MakeShared<FJsonValueObject>(Json));
                }
                SlotJson->SetArrayField(TEXT("segments"), Segments);
                Slots.Add(MakeShared<FJsonValueObject>(SlotJson));
            }
            Result->SetArrayField(TEXT("slots"), Slots);

            TArray<TSharedPtr<FJsonValue>> Notifies;
            for (const FAnimNotifyEvent& Notify : Montage->Notifies)
            {
                TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
                Json->SetStringField(TEXT("name"), Notify.GetNotifyEventName().ToString());
                Json->SetNumberField(TEXT("time"), Notify.GetTime());
                Json->SetNumberField(TEXT("duration"), Notify.GetDuration());
                Json->SetNumberField(TEXT("trackIndex"), Notify.TrackIndex);
                Json->SetStringField(TEXT("notifyClass"), Notify.Notify ? Notify.Notify->GetClass()->GetPathName() : FString());
                Json->SetStringField(TEXT("notifyStateClass"), Notify.NotifyStateClass ? Notify.NotifyStateClass->GetClass()->GetPathName() : FString());
                Notifies.Add(MakeShared<FJsonValueObject>(Json));
            }
            Result->SetArrayField(TEXT("notifies"), Notifies);
            return Result;
        }

        TSharedRef<FJsonObject> NiagaraSnapshot(UNiagaraSystem* System)
        {
            TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("assetPath"), System->GetPathName());
            Result->SetBoolField(TEXT("valid"), System->IsValid());
            Result->SetBoolField(TEXT("readyToRun"), System->IsReadyToRun());
            Result->SetNumberField(TEXT("warmupTime"), System->GetWarmupTime());
            Result->SetNumberField(TEXT("warmupTickCount"), System->GetWarmupTickCount());
            Result->SetNumberField(TEXT("warmupTickDelta"), System->GetWarmupTickDelta());

            TArray<TSharedPtr<FJsonValue>> Parameters;
            for (const FNiagaraVariableWithOffset& Parameter : System->GetExposedParameters().ReadParameterVariables())
            {
                TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
                Json->SetStringField(TEXT("name"), Parameter.GetName().ToString());
                Json->SetStringField(TEXT("type"), Parameter.GetType().GetName());
                Json->SetNumberField(TEXT("offset"), Parameter.Offset);
                Parameters.Add(MakeShared<FJsonValueObject>(Json));
            }
            Result->SetArrayField(TEXT("exposedParameters"), Parameters);

            TArray<TSharedPtr<FJsonValue>> Emitters;
            for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
            {
                TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
                Json->SetStringField(TEXT("id"), Handle.GetId().ToString(EGuidFormats::DigitsWithHyphens));
                Json->SetStringField(TEXT("name"), Handle.GetName().ToString());
                Json->SetStringField(TEXT("uniqueInstanceName"), Handle.GetUniqueInstanceName());
                Json->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());
                Json->SetStringField(TEXT("instance"), Handle.GetInstance() ? Handle.GetInstance()->GetPathName() : FString());
                Emitters.Add(MakeShared<FJsonValueObject>(Json));
            }
            Result->SetArrayField(TEXT("emitters"), Emitters);
            return Result;
        }

        bool SpecializedSnapshot(UObject* Asset, TSharedRef<FJsonObject>& OutResult, FProtocolError& OutError)
        {
            if (UMaterialInterface* Material = Cast<UMaterialInterface>(Asset))
            {
                OutResult->SetObjectField(TEXT("material"), MaterialSnapshot(Material));
                return true;
            }
            if (UAnimMontage* Montage = Cast<UAnimMontage>(Asset))
            {
                OutResult->SetObjectField(TEXT("animMontage"), MontageSnapshot(Montage));
                return true;
            }
            if (UNiagaraSystem* Niagara = Cast<UNiagaraSystem>(Asset))
            {
                OutResult->SetObjectField(TEXT("niagaraSystem"), NiagaraSnapshot(Niagara));
                return true;
            }
            if (Asset->IsA<UBlueprint>() || Asset->IsA<UWorld>())
            {
                TArray<FString> BlueprintFacets;
                if (Asset->IsA<UWidgetBlueprint>()) BlueprintFacets = {TEXT("asset"), TEXT("compile"), TEXT("variables"), TEXT("graphs"), TEXT("umg")};
                else if (Asset->IsA<UAnimBlueprint>()) BlueprintFacets = {TEXT("asset"), TEXT("compile"), TEXT("variables"), TEXT("graphs"), TEXT("anim")};
                else if (Asset->IsA<UWorld>()) BlueprintFacets = {TEXT("levelBlueprint")};
                TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>();
                if (!FBlueprintInspection::Inspect(Asset->GetPathName(), BlueprintFacets, 0, 500, Snapshot, OutError)) return false;
                OutResult->SetObjectField(TEXT("blueprint"), Snapshot);
                return true;
            }
            if (Asset->IsA<UUserDefinedStruct>() || Asset->IsA<UUserDefinedEnum>())
            {
                TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
                Json->SetStringField(TEXT("assetPath"), Asset->GetPathName());
                Json->SetStringField(TEXT("classPath"), Asset->GetClass()->GetPathName());
                OutResult->SetObjectField(TEXT("blueprintType"), Json);
                return true;
            }
            OutResult->SetStringField(TEXT("status"), TEXT("genericOnly"));
            OutResult->SetStringField(TEXT("classPath"), Asset->GetClass()->GetPathName());
            return true;
        }

        void ChangedFacets(const TSharedRef<FJsonObject>& Base, const TSharedRef<FJsonObject>& Target,
            TArray<TSharedPtr<FJsonValue>>& OutChanged)
        {
            TSet<FString> Keys;
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Base->Values) Keys.Add(Pair.Key);
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Target->Values) Keys.Add(Pair.Key);
            TArray<FString> Sorted = Keys.Array(); Sorted.Sort();
            for (const FString& Key : Sorted)
            {
                const TSharedPtr<FJsonValue> BaseValue = Base->Values.FindRef(Key);
                const TSharedPtr<FJsonValue> TargetValue = Target->Values.FindRef(Key);
                FString BaseJson, TargetJson;
                if (BaseValue.IsValid()) { const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BaseJson); FJsonSerializer::Serialize(BaseValue.ToSharedRef(), TEXT(""), Writer); }
                if (TargetValue.IsValid()) { const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&TargetJson); FJsonSerializer::Serialize(TargetValue.ToSharedRef(), TEXT(""), Writer); }
                if (BaseJson != TargetJson) OutChanged.Add(MakeShared<FJsonValueString>(Key));
            }
        }
    }

    bool FUnrealAssetInspection::Inspect(const FString& AssetPath, const TArray<FString>& Facets,
        const TArray<FString>& PropertyPaths, const int32 Offset, const int32 Limit,
        TSharedRef<FJsonObject>& OutResult, FProtocolError& OutError)
    {
        if (!IsInGameThread())
        {
            OutError = FProtocolError::Make(EErrorCode::InternalError,
                TEXT("Asset inspection must run on the game thread."), TEXT("FUnrealAssetInspection::Inspect"));
            return false;
        }
        for (const FString& Facet : Facets)
        {
            if (!KnownFacets.Contains(Facet))
            {
                OutError = FProtocolError::Make(EErrorCode::InvalidArgument,
                    FString::Printf(TEXT("Unknown asset inspection facet '%s'."), *Facet),
                    TEXT("FUnrealAssetInspection::Inspect"));
                OutError.AssetPath = AssetPath;
                return false;
            }
        }
        UObject* Asset = LoadAsset(AssetPath, OutError);
        if (!Asset) return false;

        TSharedRef<FJsonObject> FacetValues = MakeShared<FJsonObject>();
        if (WantsFacet(Facets, TEXT("support"))) FacetValues->SetObjectField(TEXT("support"), SupportSnapshot(Asset));
        if (WantsFacet(Facets, TEXT("generic"))) FacetValues->SetObjectField(TEXT("generic"), GenericSnapshot(Asset));
        if (WantsFacet(Facets, TEXT("properties")))
        {
            TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
            if (!PropertiesSnapshot(Asset, PropertyPaths, Offset, Limit, Properties, OutError)) return false;
            FacetValues->SetObjectField(TEXT("properties"), Properties);
        }
        const FName PackageName = Asset->GetOutermost()->GetFName();
        if (WantsFacet(Facets, TEXT("dependencies"))) FacetValues->SetObjectField(TEXT("dependencies"), PackageReferences(PackageName, false, Offset, Limit));
        if (WantsFacet(Facets, TEXT("referencers"))) FacetValues->SetObjectField(TEXT("referencers"), PackageReferences(PackageName, true, Offset, Limit));
        if (WantsFacet(Facets, TEXT("specialized")))
        {
            TSharedRef<FJsonObject> Specialized = MakeShared<FJsonObject>();
            if (!SpecializedSnapshot(Asset, Specialized, OutError)) return false;
            FacetValues->SetObjectField(TEXT("specialized"), Specialized);
        }

        OutResult->SetStringField(TEXT("assetPath"), Asset->GetPathName());
        OutResult->SetStringField(TEXT("mode"), TEXT("editor"));
        OutResult->SetStringField(TEXT("evidence"), TEXT("live-editor"));
        OutResult->SetObjectField(TEXT("facets"), FacetValues);
        OutResult->SetStringField(TEXT("structureHash"), Sha1(FacetValues));
        OutResult->SetNumberField(TEXT("offset"), Offset);
        OutResult->SetNumberField(TEXT("limit"), Limit);
        return true;
    }

    bool FUnrealAssetInspection::Compare(const FString& BaseAssetPath, const FString& TargetAssetPath,
        const TArray<FString>& Facets, const TArray<FString>& PropertyPaths, const int32 Offset, const int32 Limit,
        TSharedRef<FJsonObject>& OutResult, FProtocolError& OutError)
    {
        TSharedRef<FJsonObject> Base = MakeShared<FJsonObject>();
        TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
        if (!Inspect(BaseAssetPath, Facets, PropertyPaths, Offset, Limit, Base, OutError)) return false;
        if (!Inspect(TargetAssetPath, Facets, PropertyPaths, Offset, Limit, Target, OutError)) return false;
        const FString BaseHash = Base->GetStringField(TEXT("structureHash"));
        const FString TargetHash = Target->GetStringField(TEXT("structureHash"));
        OutResult->SetStringField(TEXT("baseAssetPath"), Base->GetStringField(TEXT("assetPath")));
        OutResult->SetStringField(TEXT("mode"), TEXT("editor"));
        OutResult->SetStringField(TEXT("evidence"), TEXT("live-editor"));
        OutResult->SetStringField(TEXT("targetAssetPath"), Target->GetStringField(TEXT("assetPath")));
        OutResult->SetStringField(TEXT("baseStructureHash"), BaseHash);
        OutResult->SetStringField(TEXT("targetStructureHash"), TargetHash);
        OutResult->SetBoolField(TEXT("identical"), BaseHash == TargetHash);
        const TSharedPtr<FJsonObject>* BaseFacets = nullptr; const TSharedPtr<FJsonObject>* TargetFacets = nullptr;
        Base->TryGetObjectField(TEXT("facets"), BaseFacets); Target->TryGetObjectField(TEXT("facets"), TargetFacets);
        TArray<TSharedPtr<FJsonValue>> Changed;
        if (BaseFacets && TargetFacets) ChangedFacets((*BaseFacets).ToSharedRef(), (*TargetFacets).ToSharedRef(), Changed);
        OutResult->SetArrayField(TEXT("changedFacets"), Changed);
        OutResult->SetObjectField(TEXT("base"), Base);
        OutResult->SetObjectField(TEXT("target"), Target);
        return true;
    }

    bool FUnrealAssetInspection::FindReferencers(const FString& AssetPath, const bool bRecursive,
        const int32 Offset, const int32 Limit, TSharedRef<FJsonObject>& OutResult, FProtocolError& OutError)
    {
        if (!IsInGameThread())
        {
            OutError = FProtocolError::Make(EErrorCode::InternalError,
                TEXT("Referencer search must run on the game thread."), TEXT("FUnrealAssetInspection::FindReferencers"));
            return false;
        }
        FName TargetPackage(*PackageNameForPath(AssetPath));
        if (TargetPackage.IsNone() || !FPackageName::IsValidLongPackageName(TargetPackage.ToString()))
        {
            OutError = FProtocolError::Make(EErrorCode::InvalidArgument,
                TEXT("assetPath must be an Unreal object path or long package name."),
                TEXT("FUnrealAssetInspection::FindReferencers"));
            OutError.AssetPath = AssetPath;
            return false;
        }
        IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        TSet<FName> Visited; TArray<FName> Queue = {TargetPackage}; TArray<FName> Results;
        while (Queue.Num() > 0)
        {
            const FName Current = Queue[0]; Queue.RemoveAt(0);
            if (Visited.Contains(Current)) continue;
            Visited.Add(Current);
            TArray<FName> Direct;
            Registry.GetReferencers(Current, Direct, UE::AssetRegistry::EDependencyCategory::Package);
            Direct.Sort(FNameLexicalLess());
            for (const FName& Package : Direct)
            {
                if (Package == TargetPackage || Results.Contains(Package)) continue;
                Results.Add(Package);
                if (bRecursive) Queue.Add(Package);
            }
            if (!bRecursive) break;
        }
        Results.Sort(FNameLexicalLess());
        TArray<TSharedPtr<FJsonValue>> Values;
        for (const FName& Package : Results)
        {
            TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("packageName"), Package.ToString());
            TArray<FAssetData> Assets; Registry.GetAssetsByPackageName(Package, Assets, true);
            TArray<TSharedPtr<FJsonValue>> AssetValues;
            for (const FAssetData& Asset : Assets)
            {
                TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
                Json->SetStringField(TEXT("assetPath"), Asset.ObjectPath.ToString());
                Json->SetStringField(TEXT("className"), Asset.AssetClass.ToString());
                AssetValues.Add(MakeShared<FJsonValueObject>(Json));
            }
            Item->SetArrayField(TEXT("assets"), AssetValues); Values.Add(MakeShared<FJsonValueObject>(Item));
        }
        OutResult->SetStringField(TEXT("targetPackage"), TargetPackage.ToString());
        OutResult->SetStringField(TEXT("mode"), TEXT("editor"));
        OutResult->SetStringField(TEXT("evidence"), TEXT("asset-registry"));
        OutResult->SetBoolField(TEXT("recursive"), bRecursive);
        OutResult->SetObjectField(TEXT("referencers"), PageArray(Values, Offset, Limit));
        return true;
    }
}
