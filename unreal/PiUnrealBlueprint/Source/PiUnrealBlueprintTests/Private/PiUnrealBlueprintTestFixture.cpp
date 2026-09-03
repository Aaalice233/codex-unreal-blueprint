#if WITH_DEV_AUTOMATION_TESTS

#include "PiUnrealBlueprintTestFixture.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Blueprint.h"
#include "Engine/UserDefinedEnum.h"
#include "Engine/UserDefinedStruct.h"
#include "Engine/World.h"
#include "Factories/AnimBlueprintFactory.h"
#include "Factories/WorldFactory.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFilemanager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"

namespace PiUnrealBlueprintTests
{
    namespace
    {
        FString Sanitize(const FString& Input)
        {
            FString Result = Input;
            for (TCHAR& Character : Result)
            {
                if (!FChar::IsAlnum(Character) && Character != TEXT('_')) Character = TEXT('_');
            }
            return Result;
        }
    }

    FScopedFixture::FScopedFixture(const FString& SuiteName)
    {
        FString RequestedRunId = FPlatformMisc::GetEnvironmentVariable(TEXT("PI_UE_AUTOMATION_RUN_ID"));
        if (RequestedRunId.IsEmpty()) RequestedRunId = TEXT("local");
        RunId = Sanitize(RequestedRunId + TEXT("_") + SuiteName + TEXT("_")
            + FGuid::NewGuid().ToString(EGuidFormats::Digits));
        Root = TEXT("/Game/PiAutomation/") + RunId;
    }

    FScopedFixture::~FScopedFixture()
    {
        FString Error;
        if (!Cleanup(&Error))
        {
            UE_LOG(LogTemp, Error, TEXT("PiUnrealBlueprint fixture cleanup failed for '%s': %s"), *Root, *Error);
        }
    }

    FString FScopedFixture::Package(const FString& Leaf) const
    {
        return Root + TEXT("/") + Sanitize(Leaf);
    }

    FString FScopedFixture::ObjectPath(const FString& PackageName)
    {
        return PackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(PackageName);
    }

    UObject* FScopedFixture::Track(UObject* Asset)
    {
        if (Asset) Assets.AddUnique(TWeakObjectPtr<UObject>(Asset));
        return Asset;
    }

    UBlueprint* FScopedFixture::CreateBlueprint(const FString& Leaf, UClass* ParentClass)
    {
        const FBlueprintOperationResult Result = PiUnrealBlueprint::FBlueprintAssetOperations::Create(
            Package(Leaf), PiUnrealBlueprint::EBlueprintAssetKind::Blueprint,
            ParentClass ? ParentClass : AActor::StaticClass());
        return Result.bSuccess ? Cast<UBlueprint>(Track(LoadObject<UObject>(nullptr, *ObjectPath(Package(Leaf))))) : nullptr;
    }

    UUserDefinedStruct* FScopedFixture::CreateStruct(const FString& Leaf)
    {
        const FBlueprintOperationResult Result = PiUnrealBlueprint::FBlueprintAssetOperations::Create(
            Package(Leaf), PiUnrealBlueprint::EBlueprintAssetKind::UserDefinedStruct, nullptr);
        return Result.bSuccess ? Cast<UUserDefinedStruct>(Track(LoadObject<UObject>(nullptr, *ObjectPath(Package(Leaf))))) : nullptr;
    }

    UUserDefinedEnum* FScopedFixture::CreateEnum(const FString& Leaf)
    {
        const FBlueprintOperationResult Result = PiUnrealBlueprint::FBlueprintAssetOperations::Create(
            Package(Leaf), PiUnrealBlueprint::EBlueprintAssetKind::UserDefinedEnum, nullptr);
        return Result.bSuccess ? Cast<UUserDefinedEnum>(Track(LoadObject<UObject>(nullptr, *ObjectPath(Package(Leaf))))) : nullptr;
    }

    UWidgetBlueprint* FScopedFixture::CreateWidgetBlueprint(const FString& Leaf)
    {
        const FString PackageName = Package(Leaf);
        UPackage* AssetPackage = CreatePackage(*PackageName);
        UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
        UObject* Asset = Factory->FactoryCreateNew(UWidgetBlueprint::StaticClass(), AssetPackage,
            FName(*FPackageName::GetLongPackageAssetName(PackageName)), RF_Public | RF_Standalone | RF_Transactional,
            nullptr, GWarn);
        if (Asset)
        {
            FAssetRegistryModule::AssetCreated(Asset);
            AssetPackage->MarkPackageDirty();
        }
        return Cast<UWidgetBlueprint>(Track(Asset));
    }

    USkeleton* FScopedFixture::FindAnySkeleton()
    {
        FARFilter Filter;
        Filter.ClassNames.Add(USkeleton::StaticClass()->GetFName());
        Filter.bRecursiveClasses = true;
        TArray<FAssetData> AssetsFound;
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssets(Filter, AssetsFound);
        AssetsFound.Sort([](const FAssetData& A, const FAssetData& B) { return A.ObjectPath.LexicalLess(B.ObjectPath); });
        for (const FAssetData& Asset : AssetsFound)
        {
            if (USkeleton* Skeleton = Cast<USkeleton>(Asset.GetAsset())) return Skeleton;
        }
        return nullptr;
    }

    UAnimBlueprint* FScopedFixture::CreateAnimBlueprint(const FString& Leaf)
    {
        USkeleton* Skeleton = FindAnySkeleton();
        if (!Skeleton) return nullptr;
        const FString PackageName = Package(Leaf);
        UPackage* AssetPackage = CreatePackage(*PackageName);
        UAnimBlueprintFactory* Factory = NewObject<UAnimBlueprintFactory>();
        Factory->TargetSkeleton = Skeleton;
        UObject* Asset = Factory->FactoryCreateNew(UAnimBlueprint::StaticClass(), AssetPackage,
            FName(*FPackageName::GetLongPackageAssetName(PackageName)), RF_Public | RF_Standalone | RF_Transactional,
            nullptr, GWarn);
        if (Asset)
        {
            FAssetRegistryModule::AssetCreated(Asset);
            AssetPackage->MarkPackageDirty();
        }
        return Cast<UAnimBlueprint>(Track(Asset));
    }

    UWorld* FScopedFixture::CreateWorld(const FString& Leaf)
    {
        const FString PackageName = Package(Leaf);
        UPackage* AssetPackage = CreatePackage(*PackageName);
        UWorldFactory* Factory = NewObject<UWorldFactory>();
        UObject* Asset = Factory->FactoryCreateNew(UWorld::StaticClass(), AssetPackage,
            FName(*FPackageName::GetLongPackageAssetName(PackageName)), RF_Public | RF_Standalone | RF_Transactional,
            nullptr, GWarn);
        if (Asset)
        {
            FAssetRegistryModule::AssetCreated(Asset);
            AssetPackage->MarkPackageDirty();
        }
        return Cast<UWorld>(Track(Asset));
    }

    bool FScopedFixture::Save(UObject* Asset, FString& OutFilename)
    {
        OutFilename.Reset();
        if (!Asset || !FPackageName::TryConvertLongPackageNameToFilename(
            Asset->GetOutermost()->GetName(), OutFilename, FPackageName::GetAssetPackageExtension())) return false;
        return UPackage::SavePackage(Asset->GetOutermost(), Asset, RF_Standalone, *OutFilename, GError, nullptr,
            false, true, SAVE_None);
    }

    bool FScopedFixture::UnloadAndReload(const FString& PackageName, UObject*& OutAsset)
    {
        OutAsset = nullptr;
        UPackage* Existing = FindPackage(nullptr, *PackageName);
        if (!Existing) return false;
        Existing->SetDirtyFlag(false);
        Assets.RemoveAll([&PackageName](const TWeakObjectPtr<UObject>& Asset)
        {
            return !Asset.IsValid() || Asset->GetOutermost()->GetName() == PackageName;
        });
        TArray<UPackage*> PackagesToUnload = { Existing };
        if (!UPackageTools::UnloadPackages(PackagesToUnload)) return false;
        CollectGarbage(RF_NoFlags);
        UPackage* Reloaded = LoadPackage(nullptr, *PackageName, LOAD_None);
        OutAsset = Reloaded ? FindObject<UObject>(Reloaded, *FPackageName::GetLongPackageAssetName(PackageName)) : nullptr;
        Track(OutAsset);
        return OutAsset != nullptr;
    }

    TSharedRef<FJsonObject> FScopedFixture::Operation(const FString& Name, const TCHAR* FieldName, const FString& FieldValue)
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("operation"), Name);
        if (Name.StartsWith(TEXT("widget.")) || Name.StartsWith(TEXT("slot.")) || Name.StartsWith(TEXT("namedSlot."))
            || Name.StartsWith(TEXT("event.")) || Name.StartsWith(TEXT("binding.")) || Name.StartsWith(TEXT("navigation."))
            || Name.StartsWith(TEXT("accessibility.")) || Name.StartsWith(TEXT("animation.")))
        {
            Json->SetStringField(TEXT("op"), Name);
        }
        if (FieldName) Json->SetStringField(FieldName, FieldValue);
        return Json;
    }

    bool FScopedFixture::Cleanup(FString* OutError)
    {
        if (bCleaned) return true;

        TArray<UObject*> ToDelete;
        for (const TWeakObjectPtr<UObject>& WeakAsset : Assets)
        {
            if (UObject* Asset = WeakAsset.Get()) ToDelete.AddUnique(Asset);
        }
        FARFilter Filter;
        Filter.PackagePaths.Add(FName(*Root));
        Filter.bRecursivePaths = true;
        TArray<FAssetData> RegisteredAssets;
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssets(Filter, RegisteredAssets);
        for (const FAssetData& AssetData : RegisteredAssets)
        {
            if (UObject* Asset = AssetData.GetAsset()) ToDelete.AddUnique(Asset);
        }
        if (ToDelete.Num() > 0 && ObjectTools::DeleteObjectsUnchecked(ToDelete) != ToDelete.Num())
        {
            if (OutError) *OutError = TEXT("Not every registered fixture asset could be deleted.");
            return false;
        }

        FString Directory;
        if (!FPackageName::TryConvertLongPackageNameToFilename(Root, Directory))
        {
            if (OutError) *OutError = TEXT("Fixture package root could not be converted to a filesystem path.");
            return false;
        }
        IFileManager::Get().IterateDirectoryRecursively(*Directory,
            [](const TCHAR* FilenameOrDirectory, const bool bIsDirectory)
            {
                if (!bIsDirectory) FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(FilenameOrDirectory, false);
                return true;
            });
        if (IFileManager::Get().DirectoryExists(*Directory)
            && !IFileManager::Get().DeleteDirectory(*Directory, false, true))
        {
            if (OutError) *OutError = FString::Printf(TEXT("Fixture directory remains on disk: %s"), *Directory);
            return false;
        }
        CollectGarbage(RF_NoFlags);
        bCleaned = true;
        return true;
    }

    FScopedDirectory::FScopedDirectory(const FString& InPath)
        : Path(InPath)
    {
        IFileManager::Get().MakeDirectory(*Path, true);
    }

    FScopedDirectory::~FScopedDirectory()
    {
        FString Error;
        if (!Cleanup(&Error))
        {
            UE_LOG(LogTemp, Error, TEXT("PiUnrealBlueprint temporary directory cleanup failed: %s"), *Error);
        }
    }

    bool FScopedDirectory::Cleanup(FString* OutError)
    {
        if (bCleaned) return true;
        IFileManager::Get().IterateDirectoryRecursively(*Path,
            [](const TCHAR* FilenameOrDirectory, const bool bIsDirectory)
            {
                if (!bIsDirectory) FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(FilenameOrDirectory, false);
                return true;
            });
        if (IFileManager::Get().DirectoryExists(*Path)
            && !IFileManager::Get().DeleteDirectory(*Path, false, true))
        {
            if (OutError) *OutError = FString::Printf(TEXT("Directory remains on disk: %s"), *Path);
            return false;
        }
        bCleaned = true;
        return true;
    }
}

#endif
