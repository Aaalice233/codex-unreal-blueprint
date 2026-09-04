#pragma once

#include "CoreMinimal.h"
#include "CodexUnrealBlueprintSourceControl.h"

class UPackage;

namespace CodexUnrealBlueprint
{
    struct CODEXUNREALBLUEPRINTCORE_API FTypeReferenceRequirement
    {
        FString ObjectPath;
        FString ExpectedClassPath;
        int32 OperationIndex = INDEX_NONE;
        bool bAllowNull = false;
    };

    struct CODEXUNREALBLUEPRINTCORE_API FPreflightRequest
    {
        TArray<FString> TargetPackageNames;
        TArray<FString> AdditionalImpactPackageNames;
        TArray<FString> CompilePackageNames;
        TMap<FString, TArray<int32>> OperationIndicesByPackage;
        TMap<FString, TArray<FString>> ReferencedFromByPackage;
        TArray<FTypeReferenceRequirement> TypeReferences;
        TMap<FString, FString> ExpectedStateHashes;
        TMap<FString, FString> ExpectedStructureHashes;
        TMap<FString, uint64> EstimatedNewPackageBytes;
        TFunction<bool(const FString& PackageName, FString& OutHash, FString& OutError)> StateHashResolver;
        TFunction<bool()> IsCancellationRequested;
        TFunction<void(int32 Completed, int32 Total, const FString& Message, const FString& PackageName)> ReportProgress;
        TFunction<void()> Heartbeat;
        TArray<FString> AllowedExternalPackageRoots;
        double ImpactDiscoveryDurationMs = 0.0;
        int32 AssetRegistryReferencerCount = 0;
        int32 MaximumImpactPackageCount = 512;
        uint64 MinimumFreeSpaceReserveBytes = 256ull * 1024ull * 1024ull;
    };

    struct CODEXUNREALBLUEPRINTCORE_API FImpactPackage
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
        FString AssetPath;
        FString ActualStructureHash;
        bool bStructureHashMatched = false;
        bool bHasStructureExpectation = false;
        bool bDirectWrite = false;
        bool bCompileCheck = false;
        bool bReferenceCheck = false;
        bool bLoadedByPreflight = false;
        double CheckDurationMs = 0.0;
        TArray<int32> OperationIndices;
        TArray<FString> ReferencedFrom;
        UPackage* Package = nullptr;
    };

    struct CODEXUNREALBLUEPRINTCORE_API FPreflightIssue
    {
        FString Code;
        FString Message;
        FString PackageName;
        FString ReferencePath;
        int32 OperationIndex = INDEX_NONE;
    };

    struct CODEXUNREALBLUEPRINTCORE_API FPreflightResult
    {
        bool bSucceeded = false;
        uint64 RequiredBytes = 0;
        uint64 FreeBytes = 0;
        TArray<FImpactPackage> ImpactPackages;
        TArray<FString> CompileOrder;
        FSourceControlResult SourceControl;
        TArray<FPreflightIssue> Issues;
        double TotalDurationMs = 0.0;
        double ImpactDiscoveryDurationMs = 0.0;
        double PackageChecksDurationMs = 0.0;
        double TypeReferenceChecksDurationMs = 0.0;
        double SourceControlDurationMs = 0.0;
        double DiskSpaceCheckDurationMs = 0.0;
        double CompileOrderDurationMs = 0.0;
        int32 AssetRegistryReferencerCount = 0;
        int32 LoadedPackageCount = 0;
        int32 DirectWritePackageCount = 0;
        int32 CompileCheckPackageCount = 0;
        int32 ReferenceCheckPackageCount = 0;
    };

    class CODEXUNREALBLUEPRINTCORE_API FWritePreflight
    {
    public:
        static FPreflightResult Run(const FPreflightRequest& Request);
        static bool ComputePackageStateHash(const FString& PackageName, FString& OutHash, FString& OutError);
        static bool ResolvePackageFilename(const FString& PackageName, bool bExisting, FString& OutFilename);

    private:
        static void BuildCompileOrder(const TArray<FString>& PackageNames, TArray<FString>& OutOrder);
    };
}
