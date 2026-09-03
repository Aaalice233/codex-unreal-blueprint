#pragma once

#include "CoreMinimal.h"
#include "PiUnrealBlueprintSourceControl.h"

class UPackage;

namespace PiUnrealBlueprint
{
    struct PIUNREALBLUEPRINTCORE_API FTypeReferenceRequirement
    {
        FString ObjectPath;
        FString ExpectedClassPath;
        int32 OperationIndex = INDEX_NONE;
        bool bAllowNull = false;
    };

    struct PIUNREALBLUEPRINTCORE_API FPreflightRequest
    {
        TArray<FString> TargetPackageNames;
        TArray<FString> AdditionalImpactPackageNames;
        TArray<FString> CompilePackageNames;
        TArray<FTypeReferenceRequirement> TypeReferences;
        TMap<FString, FString> ExpectedStateHashes;
        TMap<FString, uint64> EstimatedNewPackageBytes;
        TFunction<bool(const FString& PackageName, FString& OutHash, FString& OutError)> StateHashResolver;
        TArray<FString> AllowedExternalPackageRoots;
        uint64 MinimumFreeSpaceReserveBytes = 256ull * 1024ull * 1024ull;
    };

    struct PIUNREALBLUEPRINTCORE_API FImpactPackage
    {
        FString PackageName;
        FString Filename;
        bool bExistsOnDisk = false;
        bool bWasLoaded = false;
        bool bWasDirty = false;
        bool bReadOnly = false;
        uint64 EstimatedWriteBytes = 0;
        FString BeforeHash;
        FString ExpectedHash;
        UPackage* Package = nullptr;
    };

    struct PIUNREALBLUEPRINTCORE_API FPreflightIssue
    {
        FString Code;
        FString Message;
        FString PackageName;
        FString ReferencePath;
        int32 OperationIndex = INDEX_NONE;
    };

    struct PIUNREALBLUEPRINTCORE_API FPreflightResult
    {
        bool bSucceeded = false;
        uint64 RequiredBytes = 0;
        uint64 FreeBytes = 0;
        TArray<FImpactPackage> ImpactPackages;
        TArray<FString> CompileOrder;
        FSourceControlResult SourceControl;
        TArray<FPreflightIssue> Issues;
    };

    class PIUNREALBLUEPRINTCORE_API FWritePreflight
    {
    public:
        static FPreflightResult Run(const FPreflightRequest& Request);
        static bool ComputePackageStateHash(const FString& PackageName, FString& OutHash, FString& OutError);
        static bool ResolvePackageFilename(const FString& PackageName, bool bExisting, FString& OutFilename);

    private:
        static void BuildCompileOrder(const TArray<FString>& PackageNames, TArray<FString>& OutOrder);
    };
}
