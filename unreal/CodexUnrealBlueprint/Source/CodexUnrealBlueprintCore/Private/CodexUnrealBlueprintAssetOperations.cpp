#include "CodexUnrealBlueprintAssetOperations.h"

#include "AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Engine/Blueprint.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Engine/UserDefinedEnum.h"
#include "Engine/UserDefinedStruct.h"
#include "Engine/World.h"
#include "IAssetTools.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/EnumEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "CodexUnrealBlueprintTypeSystem.h"
#include "Serialization/ArchiveReplaceObjectRef.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"

namespace CodexUnrealBlueprint
{
    namespace
    {
        FBlueprintOperationResult AssetFailure(const FString& Code, const FString& Message, const FString& AssetPath,
            const int32 OperationIndex, const FString& Callsite, const TArray<FString>& Details = TArray<FString>())
        {
            return FBlueprintOperationResult::Failure(Code, Message, AssetPath, OperationIndex, Callsite, Details);
        }

        FBlueprintOperationResult AssetWrongThread(const FString& AssetPath, const int32 OperationIndex,
            const FString& Callsite)
        {
            return AssetFailure(TEXT("ASSET_WRONG_THREAD"),
                TEXT("Blueprint asset operations must run on the game thread."), AssetPath, OperationIndex, Callsite);
        }

        FString BlueprintPath(UBlueprint* Blueprint)
        {
            return Blueprint ? Blueprint->GetPathName() : FString();
        }

        FGuid FindStructFieldGuid(UUserDefinedStruct* Struct, const FString& DisplayName)
        {
            for (const FStructVariableDescription& Field : FStructureEditorUtils::GetVarDesc(Struct))
            {
                if (Field.FriendlyName == DisplayName)
                {
                    return Field.VarGuid;
                }
            }
            return FGuid();
        }

        bool IsBlueprintParentValid(UBlueprint* Blueprint, UClass* NewParentClass)
        {
            return Blueprint && NewParentClass && FKismetEditorUtilities::CanCreateBlueprintOfClass(NewParentClass) &&
                (!Blueprint->GeneratedClass || !NewParentClass->IsChildOf(Blueprint->GeneratedClass));
        }
    }

    bool FBlueprintAssetOperations::SplitPackagePath(const FString& PackagePath, FString& OutPackageName,
        FString& OutAssetName, FBlueprintOperationError& OutError, const int32 OperationIndex)
    {
        if (!IsInGameThread())
        {
            OutError = AssetWrongThread(PackagePath, OperationIndex,
                TEXT("FBlueprintAssetOperations::SplitPackagePath")).Error.GetValue();
            return false;
        }
        OutPackageName = FPackageName::ObjectPathToPackageName(PackagePath);
        if (!FPackageName::IsValidLongPackageName(OutPackageName, true))
        {
            OutError.Code = TEXT("ASSET_PATH_INVALID");
            OutError.Message = FString::Printf(TEXT("'%s' is not a valid long package path."), *PackagePath);
            OutError.AssetPath = PackagePath;
            OutError.OperationIndex = OperationIndex;
            OutError.UECallsite = TEXT("FBlueprintAssetOperations::SplitPackagePath");
            return false;
        }
        OutAssetName = FPackageName::GetLongPackageAssetName(OutPackageName);
        if (OutAssetName.IsEmpty() || !FName::IsValidXName(OutAssetName, INVALID_OBJECTNAME_CHARACTERS))
        {
            OutError.Code = TEXT("ASSET_NAME_INVALID");
            OutError.Message = FString::Printf(TEXT("'%s' does not end in a valid asset name."), *PackagePath);
            OutError.AssetPath = PackagePath;
            OutError.OperationIndex = OperationIndex;
            OutError.UECallsite = TEXT("FBlueprintAssetOperations::SplitPackagePath");
            return false;
        }
        return true;
    }

    FBlueprintOperationResult FBlueprintAssetOperations::Create(const FString& PackagePath,
        const EBlueprintAssetKind Kind, UClass* ParentClass, const int32 OperationIndex)
    {
        if (!IsInGameThread()) return AssetWrongThread(PackagePath, OperationIndex, TEXT("FBlueprintAssetOperations::Create"));
        FString PackageName;
        FString AssetName;
        FBlueprintOperationError Error;
        if (!SplitPackagePath(PackagePath, PackageName, AssetName, Error, OperationIndex))
        {
            FBlueprintOperationResult Result;
            Result.Error = Error;
            return Result;
        }
        const FString ObjectPath = PackageName + TEXT(".") + AssetName;
        if (FindObject<UObject>(nullptr, *ObjectPath) != nullptr || FPackageName::DoesPackageExist(PackageName))
        {
            return AssetFailure(TEXT("ASSET_ALREADY_EXISTS"), FString::Printf(TEXT("Asset '%s' already exists."), *ObjectPath),
                ObjectPath, OperationIndex, TEXT("FBlueprintAssetOperations::Create"));
        }

        EBlueprintType BlueprintType = BPTYPE_Normal;
        UClass* EffectiveParent = ParentClass;
        if (Kind == EBlueprintAssetKind::Interface)
        {
            BlueprintType = BPTYPE_Interface;
            EffectiveParent = UInterface::StaticClass();
        }
        else if (Kind == EBlueprintAssetKind::FunctionLibrary)
        {
            BlueprintType = BPTYPE_FunctionLibrary;
            EffectiveParent = UBlueprintFunctionLibrary::StaticClass();
        }
        else if (Kind == EBlueprintAssetKind::MacroLibrary)
        {
            BlueprintType = BPTYPE_MacroLibrary;
            EffectiveParent = EffectiveParent ? EffectiveParent : UObject::StaticClass();
        }
        if (Kind != EBlueprintAssetKind::UserDefinedStruct && Kind != EBlueprintAssetKind::UserDefinedEnum
            && (EffectiveParent == nullptr
                || (Kind != EBlueprintAssetKind::Interface && Kind != EBlueprintAssetKind::MacroLibrary
                    && Kind != EBlueprintAssetKind::FunctionLibrary
                    && !FKismetEditorUtilities::CanCreateBlueprintOfClass(EffectiveParent))))
        {
            return AssetFailure(TEXT("ASSET_PARENT_INVALID"), TEXT("The requested parent class cannot be used for this Blueprint kind."),
                ObjectPath, OperationIndex, TEXT("FKismetEditorUtilities::CanCreateBlueprintOfClass"));
        }

        UPackage* Package = CreatePackage(*PackageName);
        if (Package == nullptr)
        {
            return AssetFailure(TEXT("ASSET_PACKAGE_CREATE_FAILED"), TEXT("UE failed to create the package."),
                ObjectPath, OperationIndex, TEXT("CreatePackage"));
        }
        Package->Modify();
        UObject* CreatedAsset = nullptr;
        if (Kind == EBlueprintAssetKind::UserDefinedStruct)
        {
            CreatedAsset = FStructureEditorUtils::CreateUserDefinedStruct(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
        }
        else if (Kind == EBlueprintAssetKind::UserDefinedEnum)
        {
            CreatedAsset = FEnumEditorUtils::CreateUserDefinedEnum(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
        }
        else
        {
            CreatedAsset = FKismetEditorUtilities::CreateBlueprint(EffectiveParent, Package, FName(*AssetName), BlueprintType,
                UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), TEXT("CodexUnrealBlueprint"));
        }
        if (CreatedAsset == nullptr)
        {
            return AssetFailure(TEXT("ASSET_CREATE_FAILED"), TEXT("UE failed to create the requested asset."),
                ObjectPath, OperationIndex, TEXT("FBlueprintAssetOperations::Create"));
        }
        CreatedAsset->Modify();
        FAssetRegistryModule::AssetCreated(CreatedAsset);
        Package->MarkPackageDirty();
        FBlueprintOperationResult Result = FBlueprintOperationResult::Success({CreatedAsset->GetPathName()});
        Result.Data->SetStringField(TEXT("assetPath"), CreatedAsset->GetPathName());
        Result.Data->SetStringField(TEXT("classPath"), CreatedAsset->GetClass()->GetPathName());
        return Result;
    }

    FBlueprintOperationResult FBlueprintAssetOperations::GetOrCreateLevelBlueprint(UWorld* World,
        const int32 OperationIndex)
    {
        if (!IsInGameThread()) return AssetWrongThread(FString(), OperationIndex, TEXT("FBlueprintAssetOperations::GetOrCreateLevelBlueprint"));
        if (World == nullptr || World->PersistentLevel == nullptr)
        {
            return AssetFailure(TEXT("LEVEL_WORLD_INVALID"), TEXT("A loaded world with a persistent level is required."),
                World ? World->GetPathName() : FString(), OperationIndex, TEXT("FBlueprintAssetOperations::GetOrCreateLevelBlueprint"));
        }
        ULevelScriptBlueprint* Existing = World->PersistentLevel->GetLevelScriptBlueprint(true);
        ULevelScriptBlueprint* LevelBlueprint = Existing;
        if (LevelBlueprint == nullptr)
        {
            World->Modify();
            World->PersistentLevel->Modify();
            LevelBlueprint = World->PersistentLevel->GetLevelScriptBlueprint(false);
        }
        if (LevelBlueprint == nullptr)
        {
            return AssetFailure(TEXT("LEVEL_BLUEPRINT_CREATE_FAILED"), TEXT("UE failed to create the Level Blueprint."),
                World->GetPathName(), OperationIndex, TEXT("ULevel::GetLevelScriptBlueprint"));
        }
        if (Existing == nullptr) LevelBlueprint->Modify();
        FBlueprintOperationResult Result = FBlueprintOperationResult::Success({World->GetPathName(), LevelBlueprint->GetPathName()}, Existing == nullptr);
        Result.Data->SetStringField(TEXT("assetPath"), LevelBlueprint->GetPathName());
        Result.Data->SetStringField(TEXT("owningMapPath"), World->GetPathName());
        return Result;
    }

    FBlueprintOperationResult FBlueprintAssetOperations::Duplicate(UObject* SourceAsset,
        const FString& DestinationPackagePath, const int32 OperationIndex)
    {
        if (!IsInGameThread()) return AssetWrongThread(FString(), OperationIndex, TEXT("FBlueprintAssetOperations::Duplicate"));
        if (SourceAsset == nullptr)
        {
            return AssetFailure(TEXT("ASSET_SOURCE_REQUIRED"), TEXT("Source asset is required."), FString(),
                OperationIndex, TEXT("FBlueprintAssetOperations::Duplicate"));
        }
        FString PackageName;
        FString AssetName;
        FBlueprintOperationError Error;
        if (!SplitPackagePath(DestinationPackagePath, PackageName, AssetName, Error, OperationIndex))
        {
            FBlueprintOperationResult Result;
            Result.Error = Error;
            return Result;
        }
        const FString DestinationObjectPath = PackageName + TEXT(".") + AssetName;
        if (FindObject<UObject>(nullptr, *DestinationObjectPath) || FPackageName::DoesPackageExist(PackageName))
        {
            return AssetFailure(TEXT("ASSET_ALREADY_EXISTS"), TEXT("Destination asset already exists."), DestinationObjectPath,
                OperationIndex, TEXT("FBlueprintAssetOperations::Duplicate"));
        }
        UObject* DuplicateAsset = FAssetToolsModule::GetModule().Get().DuplicateAsset(AssetName,
            FPackageName::GetLongPackagePath(PackageName), SourceAsset);
        if (DuplicateAsset == nullptr)
        {
            return AssetFailure(TEXT("ASSET_DUPLICATE_FAILED"), TEXT("AssetTools failed to duplicate the asset."),
                SourceAsset->GetPathName(), OperationIndex, TEXT("IAssetTools::DuplicateAsset"));
        }
        DuplicateAsset->Modify();
        return FBlueprintOperationResult::Success({DuplicateAsset->GetPathName()});
    }

    FBlueprintOperationResult FBlueprintAssetOperations::RenameOrMove(UObject* Asset,
        const FString& DestinationPackagePath, const int32 OperationIndex)
    {
        if (!IsInGameThread()) return AssetWrongThread(FString(), OperationIndex, TEXT("FBlueprintAssetOperations::RenameOrMove"));
        if (Asset == nullptr)
        {
            return AssetFailure(TEXT("ASSET_REQUIRED"), TEXT("Asset is required."), FString(), OperationIndex,
                TEXT("FBlueprintAssetOperations::RenameOrMove"));
        }
        FString PackageName;
        FString AssetName;
        FBlueprintOperationError Error;
        if (!SplitPackagePath(DestinationPackagePath, PackageName, AssetName, Error, OperationIndex))
        {
            FBlueprintOperationResult Result;
            Result.Error = Error;
            return Result;
        }
        const FString OldPath = Asset->GetPathName();
        const FString NewPath = PackageName + TEXT(".") + AssetName;
        if (OldPath == NewPath)
        {
            return FBlueprintOperationResult::Success({OldPath}, false);
        }
        if (FindObject<UObject>(nullptr, *NewPath) || FPackageName::DoesPackageExist(PackageName))
        {
            return AssetFailure(TEXT("ASSET_ALREADY_EXISTS"), TEXT("Destination asset already exists."), NewPath,
                OperationIndex, TEXT("FBlueprintAssetOperations::RenameOrMove"));
        }
        Asset->Modify();
        const FAssetRenameData Rename(Asset, FPackageName::GetLongPackagePath(PackageName), AssetName);
        if (!FAssetToolsModule::GetModule().Get().RenameAssets({Rename}))
        {
            return AssetFailure(TEXT("ASSET_RENAME_FAILED"), TEXT("AssetTools failed to rename or move the asset."),
                OldPath, OperationIndex, TEXT("IAssetTools::RenameAssets"));
        }
        if (UEnum* Enum = Cast<UEnum>(Asset)) FEnumEditorUtils::UpdateAfterPathChanged(Enum);
        return FBlueprintOperationResult::Success({OldPath, Asset->GetPathName()});
    }

    FBlueprintOperationResult FBlueprintAssetOperations::Delete(UObject* Asset, const int32 OperationIndex)
    {
        if (!IsInGameThread()) return AssetWrongThread(FString(), OperationIndex, TEXT("FBlueprintAssetOperations::Delete"));
        if (Asset == nullptr)
        {
            return AssetFailure(TEXT("ASSET_REQUIRED"), TEXT("Asset is required."), FString(), OperationIndex,
                TEXT("FBlueprintAssetOperations::Delete"));
        }
        const FString AssetPath = Asset->GetPathName();
        Asset->Modify();
        if (ObjectTools::DeleteObjectsUnchecked({Asset}) != 1)
        {
            return AssetFailure(TEXT("ASSET_DELETE_FAILED"), TEXT("UE did not delete exactly the requested asset."),
                AssetPath, OperationIndex, TEXT("ObjectTools::DeleteObjectsUnchecked"));
        }
        return FBlueprintOperationResult::Success({AssetPath});
    }

    FBlueprintOperationResult FBlueprintAssetOperations::ResetParent(UBlueprint* Blueprint, UClass* NewParentClass,
        const int32 OperationIndex)
    {
        if (!IsInGameThread()) return AssetWrongThread(FString(), OperationIndex, TEXT("FBlueprintAssetOperations::ResetParent"));
        if (!IsBlueprintParentValid(Blueprint, NewParentClass))
        {
            return AssetFailure(TEXT("BLUEPRINT_PARENT_INVALID"), TEXT("The new parent is invalid or would create an inheritance cycle."),
                BlueprintPath(Blueprint), OperationIndex, TEXT("FBlueprintAssetOperations::ResetParent"));
        }
        if (Blueprint->ParentClass == NewParentClass)
        {
            return FBlueprintOperationResult::Success({Blueprint->GetPathName()}, false);
        }
        Blueprint->Modify();
        Blueprint->ParentClass = NewParentClass;
        FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        return FBlueprintOperationResult::Success({Blueprint->GetPathName()});
    }

    FBlueprintOperationResult FBlueprintAssetOperations::AddInterface(UBlueprint* Blueprint, UClass* InterfaceClass,
        const int32 OperationIndex)
    {
        if (!IsInGameThread()) return AssetWrongThread(FString(), OperationIndex, TEXT("FBlueprintAssetOperations::AddInterface"));
        if (Blueprint == nullptr || InterfaceClass == nullptr || !InterfaceClass->HasAnyClassFlags(CLASS_Interface))
        {
            return AssetFailure(TEXT("INTERFACE_INVALID"), TEXT("A Blueprint and interface class are required."),
                BlueprintPath(Blueprint), OperationIndex, TEXT("FBlueprintAssetOperations::AddInterface"));
        }
        if (Blueprint->GeneratedClass && Blueprint->GeneratedClass->ImplementsInterface(InterfaceClass))
        {
            return FBlueprintOperationResult::Success({Blueprint->GetPathName()}, false);
        }
        Blueprint->Modify();
        if (!FBlueprintEditorUtils::ImplementNewInterface(Blueprint, InterfaceClass->GetFName()))
        {
            return AssetFailure(TEXT("INTERFACE_ADD_FAILED"), TEXT("UE rejected the interface implementation."),
                Blueprint->GetPathName(), OperationIndex, TEXT("FBlueprintEditorUtils::ImplementNewInterface"));
        }
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        return FBlueprintOperationResult::Success({Blueprint->GetPathName()});
    }

    FBlueprintOperationResult FBlueprintAssetOperations::RemoveInterface(UBlueprint* Blueprint,
        UClass* InterfaceClass, const bool bPreserveFunctions, const int32 OperationIndex)
    {
        if (!IsInGameThread()) return AssetWrongThread(FString(), OperationIndex, TEXT("FBlueprintAssetOperations::RemoveInterface"));
        if (Blueprint == nullptr || InterfaceClass == nullptr || !InterfaceClass->HasAnyClassFlags(CLASS_Interface))
        {
            return AssetFailure(TEXT("INTERFACE_INVALID"), TEXT("A Blueprint and interface class are required."),
                BlueprintPath(Blueprint), OperationIndex, TEXT("FBlueprintAssetOperations::RemoveInterface"));
        }
        const bool bDirectlyImplemented = Blueprint->ImplementedInterfaces.ContainsByPredicate([InterfaceClass](const FBPInterfaceDescription& Item)
        {
            return Item.Interface == InterfaceClass;
        });
        if (!bDirectlyImplemented)
        {
            return AssetFailure(TEXT("INTERFACE_NOT_DIRECT"), TEXT("The interface is not directly implemented by this Blueprint."),
                Blueprint->GetPathName(), OperationIndex, TEXT("FBlueprintAssetOperations::RemoveInterface"));
        }
        Blueprint->Modify();
        FBlueprintEditorUtils::RemoveInterface(Blueprint, InterfaceClass->GetFName(), bPreserveFunctions);
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        return FBlueprintOperationResult::Success({Blueprint->GetPathName()});
    }

    FBlueprintOperationResult FBlueprintAssetOperations::FixRedirector(UObjectRedirector* Redirector,
        const int32 OperationIndex)
    {
        if (!IsInGameThread()) return AssetWrongThread(FString(), OperationIndex, TEXT("FBlueprintAssetOperations::FixRedirector"));
        if (Redirector == nullptr || Redirector->DestinationObject == nullptr)
        {
            return AssetFailure(TEXT("REDIRECTOR_INVALID"), TEXT("A redirector with a valid destination is required."),
                Redirector ? Redirector->GetPathName() : FString(), OperationIndex, TEXT("FBlueprintAssetOperations::FixRedirector"));
        }
        FAssetRegistryModule& RegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        TArray<FName> ReferencerNames;
        RegistryModule.Get().GetReferencers(Redirector->GetOutermost()->GetFName(), ReferencerNames);
        TArray<UPackage*> ReferencerPackages;
        TArray<FString> Affected = {Redirector->GetPathName()};
        TMap<UObject*, UObject*> ReplacementMap;
        ReplacementMap.Add(Redirector, Redirector->DestinationObject);
        for (const FName ReferencerName : ReferencerNames)
        {
            UPackage* Package = LoadPackage(nullptr, *ReferencerName.ToString(), LOAD_None);
            if (Package == nullptr)
            {
                return AssetFailure(TEXT("REDIRECTOR_REFERENCER_LOAD_FAILED"),
                    FString::Printf(TEXT("Could not load redirector referencer '%s'."), *ReferencerName.ToString()),
                    Redirector->GetPathName(), OperationIndex, TEXT("LoadPackage"), Affected);
            }
            Package->Modify();
            FArchiveReplaceObjectRef<UObject> ReplaceArchive(Package, ReplacementMap, false, true, true);
            if (ReplaceArchive.GetCount() > 0) Package->MarkPackageDirty();
            ReferencerPackages.Add(Package);
            Affected.AddUnique(ReferencerName.ToString());
        }
        const TMap<FSoftObjectPath, FSoftObjectPath> SoftPathMap = {
            {FSoftObjectPath(Redirector), FSoftObjectPath(Redirector->DestinationObject)}
        };
        FAssetToolsModule::GetModule().Get().RenameReferencingSoftObjectPaths(ReferencerPackages, SoftPathMap);
        Redirector->Modify();
        Redirector->GetOutermost()->MarkPackageDirty();
        if (ObjectTools::DeleteObjectsUnchecked({Redirector}) != 1)
        {
            return AssetFailure(TEXT("REDIRECTOR_DELETE_FAILED"),
                TEXT("References were updated in memory, but UE failed to delete the redirector."),
                Redirector->GetPathName(), OperationIndex, TEXT("ObjectTools::DeleteObjectsUnchecked"), Affected);
        }
        FBlueprintOperationResult Result = FBlueprintOperationResult::Success(Affected);
        Result.Data->SetNumberField(TEXT("referencerCount"), ReferencerPackages.Num());
        return Result;
    }

    FBlueprintOperationResult FBlueprintAssetOperations::ReadClassDefaults(UBlueprint* Blueprint,
        const TArray<FString>& PropertyPaths, const int32 OperationIndex)
    {
        if (!IsInGameThread()) return AssetWrongThread(FString(), OperationIndex, TEXT("FBlueprintAssetOperations::ReadClassDefaults"));
        if (Blueprint == nullptr || Blueprint->GeneratedClass == nullptr || Blueprint->GeneratedClass->GetDefaultObject(false) == nullptr)
        {
            return AssetFailure(TEXT("CLASS_DEFAULTS_UNAVAILABLE"), TEXT("Compile the Blueprint before reading class defaults."),
                BlueprintPath(Blueprint), OperationIndex, TEXT("FBlueprintAssetOperations::ReadClassDefaults"));
        }
        UObject* Defaults = Blueprint->GeneratedClass->GetDefaultObject(false);
        FBlueprintOperationResult Result = FBlueprintOperationResult::Success({Blueprint->GetPathName()}, false);
        TSharedRef<FJsonObject> Values = MakeShared<FJsonObject>();
        for (const FString& PropertyPath : PropertyPaths)
        {
            FBlueprintOperationError Error;
            TSharedPtr<FJsonValue> Value = FBlueprintTypeSystem::GetPropertyValue(Defaults, PropertyPath,
                Error, Blueprint->GetPathName(), OperationIndex);
            if (!Value.IsValid())
            {
                Result.bSuccess = false;
                Result.Error = Error;
                return Result;
            }
            Values->SetField(PropertyPath, Value);
        }
        Result.Data->SetObjectField(TEXT("values"), Values);
        return Result;
    }

    FBlueprintOperationResult FBlueprintAssetOperations::SetClassDefault(UBlueprint* Blueprint,
        const FString& PropertyPath, const TSharedPtr<FJsonValue>& Value, const int32 OperationIndex)
    {
        if (!IsInGameThread()) return AssetWrongThread(FString(), OperationIndex, TEXT("FBlueprintAssetOperations::SetClassDefault"));
        if (Blueprint == nullptr || Blueprint->GeneratedClass == nullptr || Blueprint->GeneratedClass->GetDefaultObject(false) == nullptr)
        {
            return AssetFailure(TEXT("CLASS_DEFAULTS_UNAVAILABLE"), TEXT("Compile the Blueprint before setting class defaults."),
                BlueprintPath(Blueprint), OperationIndex, TEXT("FBlueprintAssetOperations::SetClassDefault"));
        }
        Blueprint->Modify();
        UObject* Defaults = Blueprint->GeneratedClass->GetDefaultObject(false);
        FBlueprintOperationError Error;
        if (!FBlueprintTypeSystem::SetPropertyValue(Defaults, PropertyPath, Value, Error, Blueprint->GetPathName(), OperationIndex))
        {
            FBlueprintOperationResult Result;
            Result.Error = Error;
            return Result;
        }
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        return FBlueprintOperationResult::Success({Blueprint->GetPathName()});
    }

    FBlueprintOperationResult FBlueprintAssetOperations::AddStructField(UUserDefinedStruct* Struct,
        const FString& DisplayName, const FEdGraphPinType& Type, const FString& DefaultValue, const FString& Tooltip,
        const int32 OperationIndex)
    {
        if (!IsInGameThread()) return AssetWrongThread(FString(), OperationIndex, TEXT("FBlueprintAssetOperations::AddStructField"));
        if (Struct == nullptr || DisplayName.IsEmpty() || !FStructureEditorUtils::IsUniqueVariableFriendlyName(Struct, DisplayName))
        {
            return AssetFailure(TEXT("STRUCT_FIELD_INVALID"), TEXT("Struct and a unique non-empty display name are required."),
                Struct ? Struct->GetPathName() : FString(), OperationIndex, TEXT("FBlueprintAssetOperations::AddStructField"));
        }
        FString TypeError;
        if (!FStructureEditorUtils::CanHaveAMemberVariableOfType(Struct, Type, &TypeError))
        {
            return AssetFailure(TEXT("STRUCT_FIELD_TYPE_INVALID"), TypeError, Struct->GetPathName(), OperationIndex,
                TEXT("FStructureEditorUtils::CanHaveAMemberVariableOfType"));
        }
        Struct->Modify();
        if (!FStructureEditorUtils::AddVariable(Struct, Type))
        {
            return AssetFailure(TEXT("STRUCT_FIELD_ADD_FAILED"), TEXT("UE failed to add the struct field."),
                Struct->GetPathName(), OperationIndex, TEXT("FStructureEditorUtils::AddVariable"));
        }
        TArray<FStructVariableDescription>& Fields = FStructureEditorUtils::GetVarDesc(Struct);
        const FGuid Guid = Fields.Last().VarGuid;
        if (!FStructureEditorUtils::RenameVariable(Struct, Guid, DisplayName) ||
            !FStructureEditorUtils::ChangeVariableDefaultValue(Struct, Guid, DefaultValue) ||
            (!Tooltip.IsEmpty() && !FStructureEditorUtils::ChangeVariableTooltip(Struct, Guid, Tooltip)))
        {
            FStructureEditorUtils::RemoveVariable(Struct, Guid);
            return AssetFailure(TEXT("STRUCT_FIELD_CONFIGURE_FAILED"), TEXT("UE rejected the struct field name, default, or tooltip."),
                Struct->GetPathName(), OperationIndex, TEXT("FBlueprintAssetOperations::AddStructField"));
        }
        FStructureEditorUtils::CompileStructure(Struct);
        FBlueprintOperationResult Result = FBlueprintOperationResult::Success({Struct->GetPathName()});
        Result.Data->SetStringField(TEXT("fieldGuid"), Guid.ToString(EGuidFormats::DigitsWithHyphens));
        return Result;
    }

    FBlueprintOperationResult FBlueprintAssetOperations::UpdateStructField(UUserDefinedStruct* Struct,
        const FGuid& FieldGuid, const FString& DisplayName, const FEdGraphPinType& Type, const FString& DefaultValue,
        const FString& Tooltip, const int32 OperationIndex)
    {
        if (!IsInGameThread()) return AssetWrongThread(FString(), OperationIndex, TEXT("FBlueprintAssetOperations::UpdateStructField"));
        if (Struct == nullptr || !FieldGuid.IsValid() || FStructureEditorUtils::GetVarDescByGuid(Struct, FieldGuid) == nullptr)
        {
            return AssetFailure(TEXT("STRUCT_FIELD_NOT_FOUND"), TEXT("The struct field GUID was not found."),
                Struct ? Struct->GetPathName() : FString(), OperationIndex, TEXT("FBlueprintAssetOperations::UpdateStructField"));
        }
        const FGuid NameOwner = FindStructFieldGuid(Struct, DisplayName);
        if (DisplayName.IsEmpty() || (NameOwner.IsValid() && NameOwner != FieldGuid))
        {
            return AssetFailure(TEXT("STRUCT_FIELD_NAME_INVALID"), TEXT("The struct field display name is empty or already used."),
                Struct->GetPathName(), OperationIndex, TEXT("FBlueprintAssetOperations::UpdateStructField"));
        }
        FString TypeError;
        if (!FStructureEditorUtils::CanHaveAMemberVariableOfType(Struct, Type, &TypeError))
        {
            return AssetFailure(TEXT("STRUCT_FIELD_TYPE_INVALID"), TypeError, Struct->GetPathName(), OperationIndex,
                TEXT("FStructureEditorUtils::CanHaveAMemberVariableOfType"));
        }
        Struct->Modify();
        const FStructVariableDescription* Description = FStructureEditorUtils::GetVarDescByGuid(Struct, FieldGuid);
        if (Description->FriendlyName != DisplayName
            && !FStructureEditorUtils::RenameVariable(Struct, FieldGuid, DisplayName))
        {
            return AssetFailure(TEXT("STRUCT_FIELD_UPDATE_FAILED"), TEXT("UE rejected the struct field name."),
                Struct->GetPathName(), OperationIndex, TEXT("FStructureEditorUtils::RenameVariable"));
        }
        Description = FStructureEditorUtils::GetVarDescByGuid(Struct, FieldGuid);
        if (Description->ToPinType() != Type
            && !FStructureEditorUtils::ChangeVariableType(Struct, FieldGuid, Type))
        {
            return AssetFailure(TEXT("STRUCT_FIELD_UPDATE_FAILED"), TEXT("UE rejected the struct field type."),
                Struct->GetPathName(), OperationIndex, TEXT("FStructureEditorUtils::ChangeVariableType"));
        }
        Description = FStructureEditorUtils::GetVarDescByGuid(Struct, FieldGuid);
        if (Description->CurrentDefaultValue != DefaultValue && Description->DefaultValue != DefaultValue
            && !FStructureEditorUtils::ChangeVariableDefaultValue(Struct, FieldGuid, DefaultValue))
        {
            return AssetFailure(TEXT("STRUCT_FIELD_UPDATE_FAILED"), TEXT("UE rejected the struct field default value."),
                Struct->GetPathName(), OperationIndex, TEXT("FStructureEditorUtils::ChangeVariableDefaultValue"));
        }
        Description = FStructureEditorUtils::GetVarDescByGuid(Struct, FieldGuid);
        if (Description->ToolTip != Tooltip
            && !FStructureEditorUtils::ChangeVariableTooltip(Struct, FieldGuid, Tooltip))
        {
            return AssetFailure(TEXT("STRUCT_FIELD_UPDATE_FAILED"), TEXT("UE rejected the struct field tooltip."),
                Struct->GetPathName(), OperationIndex, TEXT("FStructureEditorUtils::ChangeVariableTooltip"));
        }
        FStructureEditorUtils::CompileStructure(Struct);
        return FBlueprintOperationResult::Success({Struct->GetPathName()});
    }

    FBlueprintOperationResult FBlueprintAssetOperations::RemoveStructField(UUserDefinedStruct* Struct,
        const FGuid& FieldGuid, const int32 OperationIndex)
    {
        if (!IsInGameThread()) return AssetWrongThread(FString(), OperationIndex, TEXT("FBlueprintAssetOperations::RemoveStructField"));
        if (Struct == nullptr || !FieldGuid.IsValid() || FStructureEditorUtils::GetVarDescByGuid(Struct, FieldGuid) == nullptr)
        {
            return AssetFailure(TEXT("STRUCT_FIELD_NOT_FOUND"), TEXT("The struct field GUID was not found."),
                Struct ? Struct->GetPathName() : FString(), OperationIndex, TEXT("FBlueprintAssetOperations::RemoveStructField"));
        }
        Struct->Modify();
        if (!FStructureEditorUtils::RemoveVariable(Struct, FieldGuid))
        {
            return AssetFailure(TEXT("STRUCT_FIELD_REMOVE_FAILED"), TEXT("UE failed to remove the struct field."),
                Struct->GetPathName(), OperationIndex, TEXT("FStructureEditorUtils::RemoveVariable"));
        }
        FStructureEditorUtils::CompileStructure(Struct);
        return FBlueprintOperationResult::Success({Struct->GetPathName()});
    }

    FBlueprintOperationResult FBlueprintAssetOperations::AddEnumValue(UUserDefinedEnum* Enum,
        const FString& DisplayName, const int32 OperationIndex)
    {
        if (!IsInGameThread()) return AssetWrongThread(FString(), OperationIndex, TEXT("FBlueprintAssetOperations::AddEnumValue"));
        if (Enum == nullptr || DisplayName.IsEmpty())
        {
            return AssetFailure(TEXT("ENUM_VALUE_INVALID"), TEXT("Enum and non-empty display name are required."),
                Enum ? Enum->GetPathName() : FString(), OperationIndex, TEXT("FBlueprintAssetOperations::AddEnumValue"));
        }
        Enum->Modify();
        const int32 NewIndex = Enum->NumEnums() - 1;
        FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(Enum);
        if (!FEnumEditorUtils::IsEnumeratorDisplayNameValid(Enum, NewIndex, FText::FromString(DisplayName)) ||
            !FEnumEditorUtils::SetEnumeratorDisplayName(Enum, NewIndex, FText::FromString(DisplayName)))
        {
            FEnumEditorUtils::RemoveEnumeratorFromUserDefinedEnum(Enum, NewIndex);
            return AssetFailure(TEXT("ENUM_VALUE_NAME_INVALID"), TEXT("The enum display name is invalid or already used."),
                Enum->GetPathName(), OperationIndex, TEXT("FEnumEditorUtils::SetEnumeratorDisplayName"));
        }
        FBlueprintOperationResult Result = FBlueprintOperationResult::Success({Enum->GetPathName()});
        Result.Data->SetNumberField(TEXT("valueIndex"), NewIndex);
        return Result;
    }

    FBlueprintOperationResult FBlueprintAssetOperations::RenameEnumValue(UUserDefinedEnum* Enum,
        const int32 ValueIndex, const FString& DisplayName, const int32 OperationIndex)
    {
        if (!IsInGameThread()) return AssetWrongThread(FString(), OperationIndex, TEXT("FBlueprintAssetOperations::RenameEnumValue"));
        if (Enum == nullptr || ValueIndex < 0 || ValueIndex >= Enum->NumEnums() - 1 || DisplayName.IsEmpty())
        {
            return AssetFailure(TEXT("ENUM_VALUE_NOT_FOUND"), TEXT("A valid non-MAX enum value index and display name are required."),
                Enum ? Enum->GetPathName() : FString(), OperationIndex, TEXT("FBlueprintAssetOperations::RenameEnumValue"));
        }
        const FText Name = FText::FromString(DisplayName);
        if (!FEnumEditorUtils::IsEnumeratorDisplayNameValid(Enum, ValueIndex, Name))
        {
            return AssetFailure(TEXT("ENUM_VALUE_NAME_INVALID"), TEXT("The enum display name is invalid or already used."),
                Enum->GetPathName(), OperationIndex, TEXT("FEnumEditorUtils::IsEnumeratorDisplayNameValid"));
        }
        Enum->Modify();
        if (!FEnumEditorUtils::SetEnumeratorDisplayName(Enum, ValueIndex, Name))
        {
            return AssetFailure(TEXT("ENUM_VALUE_RENAME_FAILED"), TEXT("UE failed to rename the enum value."),
                Enum->GetPathName(), OperationIndex, TEXT("FEnumEditorUtils::SetEnumeratorDisplayName"));
        }
        return FBlueprintOperationResult::Success({Enum->GetPathName()});
    }

    FBlueprintOperationResult FBlueprintAssetOperations::RemoveEnumValue(UUserDefinedEnum* Enum,
        const int32 ValueIndex, const int32 OperationIndex)
    {
        if (!IsInGameThread()) return AssetWrongThread(FString(), OperationIndex, TEXT("FBlueprintAssetOperations::RemoveEnumValue"));
        if (Enum == nullptr || ValueIndex < 0 || ValueIndex >= Enum->NumEnums() - 1)
        {
            return AssetFailure(TEXT("ENUM_VALUE_NOT_FOUND"), TEXT("A valid non-MAX enum value index is required."),
                Enum ? Enum->GetPathName() : FString(), OperationIndex, TEXT("FBlueprintAssetOperations::RemoveEnumValue"));
        }
        Enum->Modify();
        FEnumEditorUtils::RemoveEnumeratorFromUserDefinedEnum(Enum, ValueIndex);
        return FBlueprintOperationResult::Success({Enum->GetPathName()});
    }

    FBlueprintOperationResult FBlueprintAssetOperations::SetEnumBitflags(UUserDefinedEnum* Enum,
        const bool bBitflags, const int32 OperationIndex)
    {
        if (!IsInGameThread()) return AssetWrongThread(FString(), OperationIndex, TEXT("FBlueprintAssetOperations::SetEnumBitflags"));
        if (Enum == nullptr)
        {
            return AssetFailure(TEXT("ENUM_REQUIRED"), TEXT("Enum is required."), FString(), OperationIndex,
                TEXT("FBlueprintAssetOperations::SetEnumBitflags"));
        }
        if (FEnumEditorUtils::IsEnumeratorBitflagsType(Enum) == bBitflags)
        {
            return FBlueprintOperationResult::Success({Enum->GetPathName()}, false);
        }
        Enum->Modify();
        FEnumEditorUtils::SetEnumeratorBitflagsTypeState(Enum, bBitflags);
        return FBlueprintOperationResult::Success({Enum->GetPathName()});
    }
}
