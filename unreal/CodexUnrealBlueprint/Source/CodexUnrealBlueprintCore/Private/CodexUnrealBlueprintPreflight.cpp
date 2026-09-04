#include "CodexUnrealBlueprintPreflight.h"
#include "CodexUnrealBlueprintInspection.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "HAL/PlatformFilemanager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace CodexUnrealBlueprint
{
    DEFINE_LOG_CATEGORY_STATIC(LogCodexUnrealBlueprintPreflight, Log, All);

    namespace
    {
        void AddIssue(
            FPreflightResult& Result,
            const FString& Code,
            const FString& Message,
            const FString& PackageName = FString(),
            const FString& ReferencePath = FString(),
            const int32 OperationIndex = INDEX_NONE)
        {
            FPreflightIssue Issue;
            Issue.Code = Code;
            Issue.Message = Message;
            Issue.PackageName = PackageName;
            Issue.ReferencePath = ReferencePath;
            Issue.OperationIndex = OperationIndex;
            Result.Issues.Add(MoveTemp(Issue));
        }

        FString BytesToHex(const uint8* Bytes, const int32 Count)
        {
            static const TCHAR Hex[] = TEXT("0123456789abcdef");
            FString Result;
            Result.Reserve(Count * 2);
            for (int32 Index = 0; Index < Count; ++Index)
            {
                Result.AppendChar(Hex[(Bytes[Index] >> 4) & 0x0f]);
                Result.AppendChar(Hex[Bytes[Index] & 0x0f]);
            }
            return Result;
        }

        FString NormalizePackageName(const FString& Input)
        {
            FString Result = Input;
            Result.TrimStartAndEndInline();
            if (Result.Contains(TEXT(".")))
            {
                Result = FPackageName::ObjectPathToPackageName(Result);
            }
            return Result;
        }

        void VisitCompilePackage(
            const FName PackageName,
            const TSet<FName>& Included,
            IAssetRegistry& Registry,
            TSet<FName>& Visiting,
            TSet<FName>& Visited,
            TArray<FString>& OutOrder)
        {
            if (Visited.Contains(PackageName))
            {
                return;
            }
            if (Visiting.Contains(PackageName))
            {
                return;
            }
            Visiting.Add(PackageName);
            TArray<FName> Dependencies;
            Registry.GetDependencies(PackageName, Dependencies,
                UE::AssetRegistry::EDependencyCategory::Package,
                UE::AssetRegistry::FDependencyQuery());
            Dependencies.Sort(FNameLexicalLess());
            for (const FName Dependency : Dependencies)
            {
                if (Included.Contains(Dependency))
                {
                    VisitCompilePackage(Dependency, Included, Registry, Visiting, Visited, OutOrder);
                }
            }
            Visiting.Remove(PackageName);
            Visited.Add(PackageName);
            OutOrder.Add(PackageName.ToString());
        }
    }

    bool FWritePreflight::ResolvePackageFilename(const FString& PackageName, const bool bExisting, FString& OutFilename)
    {
        OutFilename.Reset();
        if (bExisting)
        {
            return FPackageName::DoesPackageExist(PackageName, nullptr, &OutFilename);
        }
        if (!FPackageName::TryConvertLongPackageNameToFilename(PackageName, OutFilename, FPackageName::GetAssetPackageExtension()))
        {
            OutFilename.Reset();
            return false;
        }
        OutFilename = FPaths::ConvertRelativePathToFull(OutFilename);
        return true;
    }

    bool FWritePreflight::ComputePackageStateHash(const FString& PackageName, FString& OutHash, FString& OutError)
    {
        OutHash.Reset();
        OutError.Reset();
        FString Filename;
        if (!FPackageName::DoesPackageExist(PackageName, nullptr, &Filename))
        {
            OutHash = TEXT("missing");
            return true;
        }
        const FMD5Hash Hash = FMD5Hash::HashFile(*Filename);
        if (!Hash.IsValid())
        {
            OutError = FString::Printf(TEXT("Failed to hash package file: %s."), *Filename);
            return false;
        }
        OutHash = BytesToHex(Hash.GetBytes(), Hash.GetSize());
        return true;
    }

    void FWritePreflight::BuildCompileOrder(const TArray<FString>& PackageNames, TArray<FString>& OutOrder)
    {
        OutOrder.Reset();
        TSet<FName> Included;
        for (const FString& PackageName : PackageNames)
        {
            Included.Add(FName(*PackageName));
        }
        TArray<FName> Sorted = Included.Array();
        Sorted.Sort(FNameLexicalLess());
        IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        TSet<FName> Visiting;
        TSet<FName> Visited;
        for (const FName PackageName : Sorted)
        {
            VisitCompilePackage(PackageName, Included, Registry, Visiting, Visited, OutOrder);
        }
    }

    FPreflightResult FWritePreflight::Run(const FPreflightRequest& Request)
    {
        const double PreflightStartedAt = FPlatformTime::Seconds();
        FPreflightResult Result;
        Result.ImpactDiscoveryDurationMs = Request.ImpactDiscoveryDurationMs;
        Result.AssetRegistryReferencerCount = Request.AssetRegistryReferencerCount;
        const auto Finish = [&Result, PreflightStartedAt]()
        {
            Result.TotalDurationMs = Result.ImpactDiscoveryDurationMs
                + (FPlatformTime::Seconds() - PreflightStartedAt) * 1000.0;
            return Result;
        };
        if (!IsInGameThread())
        {
            AddIssue(Result, TEXT("preflight.gameThreadRequired"),
                TEXT("Write preflight must run on the Unreal Game Thread."));
            return Finish();
        }

        TArray<FString> TargetPackages;
        TArray<FString> ImpactPackages;
        for (const FString& Input : Request.TargetPackageNames)
        {
            const FString PackageName = NormalizePackageName(Input);
            if (!PackageName.IsEmpty())
            {
                TargetPackages.AddUnique(PackageName);
                ImpactPackages.AddUnique(PackageName);
            }
        }
        for (const FString& Input : Request.AdditionalImpactPackageNames)
        {
            const FString PackageName = NormalizePackageName(Input);
            if (!PackageName.IsEmpty())
            {
                ImpactPackages.AddUnique(PackageName);
            }
        }
        ImpactPackages.Sort();
        if (Request.MaximumImpactPackageCount < 1 || ImpactPackages.Num() > Request.MaximumImpactPackageCount)
        {
            AddIssue(Result, TEXT("preflight.impactScopeTooLarge"),
                FString::Printf(TEXT("Preflight discovered %d affected packages; the safe limit is %d. Narrow the request or inspect the referencer graph."),
                    ImpactPackages.Num(), Request.MaximumImpactPackageCount));
            return Finish();
        }
        if (ImpactPackages.Num() == 0)
        {
            AddIssue(Result, TEXT("preflight.noImpactPackages"), TEXT("The write request has no affected packages."));
            return Finish();
        }

        TMap<FString, FString> ExpectedByPackage;
        TMap<FString, FString> ActualByPackage;
        TMap<FString, FString> AssetByPackage;
        for (const TPair<FString, FString>& Pair : Request.ExpectedStructureHashes)
        {
            const FString PackageName = FPackageName::ObjectPathToPackageName(Pair.Key);
            if (!TargetPackages.Contains(PackageName))
            {
                AddIssue(Result, TEXT("preflight.structureHashTargetNotFound"),
                    FString::Printf(TEXT("expectedStructureHashes key is not a direct operation target: %s."), *Pair.Key),
                    PackageName, Pair.Key);
                continue;
            }
            if (ExpectedByPackage.Contains(PackageName))
            {
                AddIssue(Result, TEXT("preflight.structureHashAmbiguousPackage"),
                    TEXT("Only one expected Blueprint asset is allowed per package."), PackageName, Pair.Key);
                continue;
            }
            FString Actual;
            FProtocolError HashError;
            if (!FBlueprintInspection::ComputeStructureHash(Pair.Key, Actual, HashError))
            {
                AddIssue(Result, TEXT("preflight.structureHashRead"), HashError.Message, PackageName, Pair.Key);
                continue;
            }
            ExpectedByPackage.Add(PackageName, Pair.Value);
            ActualByPackage.Add(PackageName, Actual);
            AssetByPackage.Add(PackageName, Pair.Key);
            if (!Actual.Equals(Pair.Value, ESearchCase::IgnoreCase))
                AddIssue(Result, TEXT("preflight.structureHashMismatch"),
                    FString::Printf(TEXT("structureHash does not match %s (expected %s, actual %s)."), *Pair.Key, *Pair.Value, *Actual),
                    PackageName, Pair.Key);
        }

        IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
        TArray<FString> ExistingFiles;
        TArray<FString> NewFiles;
        uint64 WriteBytes = 0;
        const double PackageChecksStartedAt = FPlatformTime::Seconds();
        for (int32 PackageIndex = 0; PackageIndex < ImpactPackages.Num(); ++PackageIndex)
        {
            const FString& PackageName = ImpactPackages[PackageIndex];
            if (Request.IsCancellationRequested && Request.IsCancellationRequested())
            {
                AddIssue(Result, TEXT("preflight.cancelled"), TEXT("Preflight was cancelled between package checks."), PackageName);
                break;
            }
            if (Request.ReportProgress)
                Request.ReportProgress(PackageIndex, ImpactPackages.Num(), TEXT("Checking affected package metadata."), PackageName);
            if (Request.Heartbeat) Request.Heartbeat();
            const double PackageStartedAt = FPlatformTime::Seconds();
            if (!FPackageName::IsValidLongPackageName(PackageName, true))
            {
                AddIssue(Result, TEXT("preflight.invalidPackageName"),
                    FString::Printf(TEXT("Invalid long package name: %s."), *PackageName), PackageName);
                continue;
            }

            FImpactPackage Impact;
            Impact.PackageName = PackageName;
            Impact.bDirectWrite = TargetPackages.Contains(PackageName);
            Impact.bCompileCheck = !Impact.bDirectWrite && Request.CompilePackageNames.Contains(PackageName);
            Impact.bReferenceCheck = !Impact.bDirectWrite && !Impact.bCompileCheck;
            Result.DirectWritePackageCount += Impact.bDirectWrite ? 1 : 0;
            Result.CompileCheckPackageCount += Impact.bCompileCheck ? 1 : 0;
            Result.ReferenceCheckPackageCount += Impact.bReferenceCheck ? 1 : 0;
            Impact.OperationIndices = Request.OperationIndicesByPackage.FindRef(PackageName);
            Impact.ReferencedFrom = Request.ReferencedFromByPackage.FindRef(PackageName);
            if (const FString* AssetPath = AssetByPackage.Find(PackageName)) Impact.AssetPath = *AssetPath;
            if (const FString* Expected = ExpectedByPackage.Find(PackageName))
            {
                Impact.bHasStructureExpectation = true;
                Impact.ExpectedHash = *Expected;
                Impact.ActualStructureHash = ActualByPackage.FindRef(PackageName);
                Impact.bStructureHashMatched = Impact.ActualStructureHash.Equals(Impact.ExpectedHash, ESearchCase::IgnoreCase);
            }
            Impact.bExistsOnDisk = FPackageName::DoesPackageExist(PackageName, nullptr, &Impact.Filename);
            if (!ResolvePackageFilename(PackageName, Impact.bExistsOnDisk, Impact.Filename))
            {
                AddIssue(Result, TEXT("preflight.packageFilename"),
                    FString::Printf(TEXT("Cannot resolve a filename for package: %s."), *PackageName), PackageName);
                continue;
            }
            const FString FullFilename = FPaths::ConvertRelativePathToFull(Impact.Filename);
            FString ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
            FPaths::NormalizeDirectoryName(ProjectRoot);
            const bool bInsideProject = FullFilename.StartsWith(ProjectRoot + TEXT("/"), ESearchCase::IgnoreCase)
                || FullFilename.StartsWith(ProjectRoot + TEXT("\\"), ESearchCase::IgnoreCase);
            bool bExplicitlyAllowed = false;
            for (const FString& AllowedRootInput : Request.AllowedExternalPackageRoots)
            {
                FString AllowedRoot = NormalizePackageName(AllowedRootInput);
                AllowedRoot.RemoveFromEnd(TEXT("/"));
                if (PackageName == AllowedRoot || PackageName.StartsWith(AllowedRoot + TEXT("/")))
                {
                    bExplicitlyAllowed = true;
                    break;
                }
            }
            if (!bInsideProject && !bExplicitlyAllowed)
            {
                AddIssue(Result, TEXT("preflight.readOnlyMount"),
                    FString::Printf(TEXT("Package is outside project Content and its mount root was not explicitly enabled: %s."), *PackageName),
                    PackageName);
            }
            Impact.Package = FindPackage(nullptr, *PackageName);
            Impact.bWasLoaded = Impact.Package != nullptr;
            if (Impact.bDirectWrite && Impact.bExistsOnDisk && Impact.Package == nullptr)
            {
                UE_LOG(LogCodexUnrealBlueprintPreflight, Display,
                    TEXT("Loading direct-write package %d/%d during preflight: %s"),
                    PackageIndex + 1, ImpactPackages.Num(), *PackageName);
                Impact.Package = LoadPackage(nullptr, *PackageName, LOAD_None);
                Impact.bLoadedByPreflight = Impact.Package != nullptr;
                if (Impact.bLoadedByPreflight) ++Result.LoadedPackageCount;
            }
            if (Impact.bDirectWrite && Impact.bExistsOnDisk && Impact.Package == nullptr)
            {
                AddIssue(Result, TEXT("preflight.packageLoad"),
                    FString::Printf(TEXT("Failed to load affected package: %s."), *PackageName), PackageName);
                continue;
            }
            Impact.bWasDirty = Impact.Package != nullptr && Impact.Package->IsDirty();
            if (Impact.bWasDirty && (Impact.bDirectWrite || Impact.bCompileCheck))
            {
                AddIssue(Result, TEXT("preflight.dirtyPackage"),
                    FString::Printf(TEXT("A package that would be written or compiled has unsaved user changes: %s."), *PackageName), PackageName);
            }
            Impact.bReadOnly = Impact.bExistsOnDisk && PlatformFile.IsReadOnly(*Impact.Filename);
            if (Impact.bDirectWrite && Impact.bExistsOnDisk)
            {
                const int64 Size = PlatformFile.FileSize(*Impact.Filename);
                Impact.EstimatedWriteBytes = Size > 0 ? static_cast<uint64>(Size) : 0;
                ExistingFiles.Add(Impact.Filename);
            }
            else if (Impact.bDirectWrite)
            {
                const uint64* Estimate = Request.EstimatedNewPackageBytes.Find(PackageName);
                Impact.EstimatedWriteBytes = Estimate != nullptr ? *Estimate : 64ull * 1024ull * 1024ull;
                NewFiles.Add(Impact.Filename);
            }
            if (Impact.bDirectWrite) WriteBytes += Impact.EstimatedWriteBytes;

            if (Impact.bDirectWrite)
            {
                FString HashError;
                const bool bHashRead = Request.StateHashResolver
                    ? Request.StateHashResolver(PackageName, Impact.BeforeHash, HashError)
                    : ComputePackageStateHash(PackageName, Impact.BeforeHash, HashError);
                if (!bHashRead)
                {
                    AddIssue(Result, TEXT("preflight.stateHashRead"), HashError, PackageName);
                }
                if (const FString* Expected = Request.ExpectedStateHashes.Find(PackageName))
                {
                    Impact.ExpectedHash = *Expected;
                    if (!Impact.BeforeHash.Equals(Impact.ExpectedHash, ESearchCase::IgnoreCase))
                    {
                        AddIssue(Result, TEXT("preflight.stateHashMismatch"),
                            FString::Printf(TEXT("expectedStateHash does not match %s (expected %s, actual %s)."),
                                *PackageName, **Expected, *Impact.BeforeHash), PackageName);
                    }
                }
            }
            Impact.CheckDurationMs = (FPlatformTime::Seconds() - PackageStartedAt) * 1000.0;
            UE_LOG(LogCodexUnrealBlueprintPreflight, Verbose,
                TEXT("Preflight package %d/%d checked in %.3f ms: %s (direct=%s compile=%s reference=%s loaded=%s)"),
                PackageIndex + 1, ImpactPackages.Num(), Impact.CheckDurationMs, *PackageName,
                Impact.bDirectWrite ? TEXT("true") : TEXT("false"),
                Impact.bCompileCheck ? TEXT("true") : TEXT("false"),
                Impact.bReferenceCheck ? TEXT("true") : TEXT("false"),
                Impact.bLoadedByPreflight ? TEXT("true") : TEXT("false"));
            Result.ImpactPackages.Add(MoveTemp(Impact));
            if (Request.ReportProgress)
                Request.ReportProgress(PackageIndex + 1, ImpactPackages.Num(), TEXT("Checked affected package metadata."), PackageName);
        }
        Result.PackageChecksDurationMs = (FPlatformTime::Seconds() - PackageChecksStartedAt) * 1000.0;
        if (Request.IsCancellationRequested && Request.IsCancellationRequested())
        {
            return Finish();
        }

        const double TypeReferencesStartedAt = FPlatformTime::Seconds();
        for (const FTypeReferenceRequirement& Requirement : Request.TypeReferences)
        {
            if (Requirement.ObjectPath.TrimStartAndEnd().IsEmpty())
            {
                if (!Requirement.bAllowNull)
                {
                    AddIssue(Result, TEXT("preflight.referenceRequired"), TEXT("A required type reference is empty."),
                        FString(), Requirement.ObjectPath, Requirement.OperationIndex);
                }
                continue;
            }
            UObject* Resolved = StaticLoadObject(UObject::StaticClass(), nullptr, *Requirement.ObjectPath);
            if (Resolved == nullptr)
            {
                AddIssue(Result, TEXT("preflight.referenceMissing"),
                    FString::Printf(TEXT("Referenced object does not exist: %s."), *Requirement.ObjectPath),
                    FString(), Requirement.ObjectPath, Requirement.OperationIndex);
                continue;
            }
            if (!Requirement.ExpectedClassPath.IsEmpty())
            {
                UClass* ExpectedClass = LoadObject<UClass>(nullptr, *Requirement.ExpectedClassPath);
                if (ExpectedClass == nullptr)
                {
                    AddIssue(Result, TEXT("preflight.expectedClassMissing"),
                        FString::Printf(TEXT("Expected reference class does not exist: %s."), *Requirement.ExpectedClassPath),
                        FString(), Requirement.ExpectedClassPath, Requirement.OperationIndex);
                }
                else if (!Resolved->IsA(ExpectedClass))
                {
                    AddIssue(Result, TEXT("preflight.referenceTypeMismatch"),
                        FString::Printf(TEXT("Reference %s is %s, expected %s."), *Requirement.ObjectPath,
                            *Resolved->GetClass()->GetPathName(), *ExpectedClass->GetPathName()),
                        FString(), Requirement.ObjectPath, Requirement.OperationIndex);
                }
            }
        }
        Result.TypeReferenceChecksDurationMs = (FPlatformTime::Seconds() - TypeReferencesStartedAt) * 1000.0;

        const double SourceControlStartedAt = FPlatformTime::Seconds();
        Result.SourceControl = FWriteSourceControl::Inspect(ExistingFiles, NewFiles);
        if (!Result.SourceControl.bSucceeded)
        {
            AddIssue(Result, TEXT("preflight.sourceControlState"), Result.SourceControl.Error);
        }
        else
        {
            if (Result.SourceControl.bProviderEnabled && !Result.SourceControl.bProviderAvailable)
            {
                AddIssue(Result, TEXT("preflight.sourceControlUnavailable"),
                    FString::Printf(TEXT("Configured Source Control provider '%s' is unavailable."), *Result.SourceControl.ProviderName));
            }
            for (const FSourceControlFileState& File : Result.SourceControl.Files)
            {
                if (!File.CheckedOutBy.IsEmpty())
                {
                    AddIssue(Result, TEXT("preflight.checkedOutOther"),
                        FString::Printf(TEXT("Affected file is checked out by %s: %s."), *File.CheckedOutBy, *File.Filename));
                }
                if (File.bReadOnly && (!Result.SourceControl.bProviderEnabled || !Result.SourceControl.bProviderAvailable || !File.bCanCheckout))
                {
                    AddIssue(Result, TEXT("preflight.readOnly"),
                        FString::Printf(TEXT("Read-only file cannot be checked out: %s."), *File.Filename));
                }
            }
        }
        Result.SourceControlDurationMs = (FPlatformTime::Seconds() - SourceControlStartedAt) * 1000.0;

        const double DiskSpaceStartedAt = FPlatformTime::Seconds();
        Result.RequiredBytes = WriteBytes + Request.MinimumFreeSpaceReserveBytes;
        uint64 TotalBytes = 0;
        if (!FPlatformMisc::GetDiskTotalAndFreeSpace(FPaths::ProjectDir(), TotalBytes, Result.FreeBytes))
        {
            AddIssue(Result, TEXT("preflight.diskSpaceUnknown"), TEXT("Failed to query free disk space for the project drive."));
        }
        else if (Result.FreeBytes < Result.RequiredBytes)
        {
            AddIssue(Result, TEXT("preflight.diskSpace"),
                FString::Printf(TEXT("Insufficient disk space: %llu bytes required, %llu bytes free."),
                Result.RequiredBytes, Result.FreeBytes));
        }
        Result.DiskSpaceCheckDurationMs = (FPlatformTime::Seconds() - DiskSpaceStartedAt) * 1000.0;

        TArray<FString> CompilePackages;
        for (const FString& Input : Request.CompilePackageNames)
        {
            const FString PackageName = NormalizePackageName(Input);
            if (!PackageName.IsEmpty())
            {
                CompilePackages.AddUnique(PackageName);
            }
        }
        for (const FString& PackageName : TargetPackages)
        {
            CompilePackages.AddUnique(PackageName);
        }
        const double CompileOrderStartedAt = FPlatformTime::Seconds();
        BuildCompileOrder(CompilePackages, Result.CompileOrder);
        Result.CompileOrderDurationMs = (FPlatformTime::Seconds() - CompileOrderStartedAt) * 1000.0;
        Result.bSucceeded = Result.Issues.Num() == 0;
        Result.TotalDurationMs = Result.ImpactDiscoveryDurationMs
            + (FPlatformTime::Seconds() - PreflightStartedAt) * 1000.0;
        UE_LOG(LogCodexUnrealBlueprintPreflight, Display,
            TEXT("Preflight completed: impacts=%d direct=%d compile=%d reference=%d registryReferencers=%d loaded=%d total=%.3fms discovery=%.3fms packageChecks=%.3fms"),
            Result.ImpactPackages.Num(), Result.DirectWritePackageCount, Result.CompileCheckPackageCount,
            Result.ReferenceCheckPackageCount, Result.AssetRegistryReferencerCount, Result.LoadedPackageCount,
            Result.TotalDurationMs, Result.ImpactDiscoveryDurationMs,
            Result.PackageChecksDurationMs);
        return Result;
    }
}
