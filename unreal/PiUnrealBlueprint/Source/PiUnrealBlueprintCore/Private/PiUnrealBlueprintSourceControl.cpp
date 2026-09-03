#include "PiUnrealBlueprintSourceControl.h"

#include "HAL/PlatformFilemanager.h"
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "ISourceControlState.h"
#include "Misc/Paths.h"
#include "SourceControlOperations.h"

namespace PiUnrealBlueprint
{
    namespace
    {
        bool ExistsAsFileOrDirectory(IPlatformFile& PlatformFile, const FString& Path)
        {
            return PlatformFile.FileExists(*Path) || PlatformFile.DirectoryExists(*Path);
        }

        FSourceControlResult ExecuteOperation(
            const TSharedRef<ISourceControlOperation, ESPMode::ThreadSafe>& Operation,
            const TArray<FString>& Files,
            const TCHAR* OperationName)
        {
            FSourceControlResult Result;
            ISourceControlModule& Module = ISourceControlModule::Get();
            ISourceControlProvider& Provider = Module.GetProvider();
            Result.bProviderEnabled = Provider.IsEnabled();
            Result.bProviderAvailable = Provider.IsAvailable();
            Result.ProviderName = Provider.GetName().ToString();
            if (Files.Num() == 0)
            {
                Result.bSucceeded = true;
                return Result;
            }
            if (!Result.bProviderEnabled || !Result.bProviderAvailable)
            {
                Result.Error = FString::Printf(TEXT("Source Control provider '%s' is not enabled and available for %s."),
                    *Result.ProviderName, OperationName);
                return Result;
            }
            if (Provider.Execute(Operation, Files, EConcurrency::Synchronous) != ECommandResult::Succeeded)
            {
                Result.Error = FString::Printf(TEXT("Source Control provider '%s' failed to %s %d file(s)."),
                    *Result.ProviderName, OperationName, Files.Num());
                return Result;
            }
            Result.bSucceeded = true;
            return Result;
        }
    }

    FSourceControlResult FWriteSourceControl::Query(const TArray<FString>& ExistingFiles, const TArray<FString>& NewFiles)
    {
        FSourceControlResult Result;
        if (!IsInGameThread())
        {
            Result.Error = TEXT("Source Control inspection must run on the Unreal Game Thread.");
            return Result;
        }
        ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();
        Result.bProviderEnabled = Provider.IsEnabled();
        Result.bProviderAvailable = Provider.IsAvailable();
        Result.ProviderName = Provider.GetName().ToString();

        IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
        TArray<FString> AllFiles = ExistingFiles;
        for (const FString& Filename : NewFiles)
        {
            AllFiles.AddUnique(Filename);
        }

        TMap<FString, TSharedRef<ISourceControlState, ESPMode::ThreadSafe>> States;
        if (Result.bProviderEnabled && Result.bProviderAvailable && AllFiles.Num() > 0)
        {
            TArray<TSharedRef<ISourceControlState, ESPMode::ThreadSafe>> QueriedStates;
            if (Provider.GetState(AllFiles, QueriedStates, EStateCacheUsage::ForceUpdate) != ECommandResult::Succeeded)
            {
                Result.Error = FString::Printf(TEXT("Source Control provider '%s' failed to refresh file state."), *Result.ProviderName);
                return Result;
            }
            for (const TSharedRef<ISourceControlState, ESPMode::ThreadSafe>& State : QueriedStates)
            {
                States.Add(FPaths::ConvertRelativePathToFull(State->GetFilename()).ToLower(), State);
            }
        }

        for (const FString& InputFilename : AllFiles)
        {
            const FString Filename = FPaths::ConvertRelativePathToFull(InputFilename);
            FSourceControlFileState File;
            File.Filename = Filename;
            File.bExists = PlatformFile.FileExists(*Filename);
            File.bReadOnly = File.bExists && PlatformFile.IsReadOnly(*Filename);
            if (const TSharedRef<ISourceControlState, ESPMode::ThreadSafe>* State = States.Find(Filename.ToLower()))
            {
                File.bSourceControlled = (*State)->IsSourceControlled();
                File.bCheckedOut = (*State)->IsCheckedOut();
                File.bAdded = (*State)->IsAdded();
                File.bCanCheckout = (*State)->CanCheckout();
                File.bCanAdd = (*State)->CanAdd();
                (*State)->IsCheckedOutOther(&File.CheckedOutBy);
            }
            File.bNeedsCheckout = File.bExists && !File.bCheckedOut
                && (File.bReadOnly || (File.bSourceControlled && File.bCanCheckout));
            Result.Files.Add(MoveTemp(File));
        }

        Result.bSucceeded = true;
        return Result;
    }

    FSourceControlResult FWriteSourceControl::Inspect(const TArray<FString>& ExistingFiles, const TArray<FString>& NewFiles)
    {
        return Query(ExistingFiles, NewFiles);
    }

    FSourceControlResult FWriteSourceControl::Checkout(const TArray<FString>& ExistingFiles)
    {
        FSourceControlResult Inspection = Query(ExistingFiles, TArray<FString>());
        if (!Inspection.bSucceeded)
        {
            return Inspection;
        }

        TArray<FString> FilesToCheckout;
        for (const FSourceControlFileState& File : Inspection.Files)
        {
            if (!File.CheckedOutBy.IsEmpty())
            {
                Inspection.bSucceeded = false;
                Inspection.Error = FString::Printf(TEXT("File is checked out by another user: %s (%s)."),
                    *File.Filename, *File.CheckedOutBy);
                return Inspection;
            }
            if (File.bNeedsCheckout)
            {
                if (!File.bCanCheckout)
                {
                    Inspection.bSucceeded = false;
                    Inspection.Error = FString::Printf(TEXT("File requires checkout but the provider does not allow it: %s."), *File.Filename);
                    return Inspection;
                }
                FilesToCheckout.Add(File.Filename);
            }
        }
        if (FilesToCheckout.Num() == 0)
        {
            Inspection.bSucceeded = true;
            return Inspection;
        }
        return ExecuteOperation(ISourceControlOperation::Create<FCheckOut>(), FilesToCheckout, TEXT("check out"));
    }

    FSourceControlResult FWriteSourceControl::MarkForAdd(const TArray<FString>& SavedNewFiles)
    {
        if (SavedNewFiles.Num() == 0)
        {
            FSourceControlResult Result;
            Result.bSucceeded = IsInGameThread();
            if (!Result.bSucceeded) Result.Error = TEXT("Source Control mark-for-add must run on the Unreal Game Thread.");
            return Result;
        }

        FSourceControlResult Inspection = Query(TArray<FString>(), SavedNewFiles);
        if (!Inspection.bSucceeded)
        {
            return Inspection;
        }
        if (!Inspection.bProviderEnabled)
        {
            Inspection.bSucceeded = true;
            return Inspection;
        }
        if (!Inspection.bProviderAvailable)
        {
            Inspection.bSucceeded = false;
            Inspection.Error = TEXT("The configured Source Control provider became unavailable before mark-for-add.");
            return Inspection;
        }

        TArray<FString> FilesToAdd;
        for (const FSourceControlFileState& File : Inspection.Files)
        {
            if (File.bAdded || File.bSourceControlled)
            {
                continue;
            }
            if (!File.bCanAdd)
            {
                Inspection.bSucceeded = false;
                Inspection.Error = FString::Printf(TEXT("Saved file cannot be marked for add: %s."), *File.Filename);
                return Inspection;
            }
            FilesToAdd.Add(File.Filename);
        }
        if (FilesToAdd.Num() == 0)
        {
            Inspection.bSucceeded = true;
            return Inspection;
        }
        return ExecuteOperation(ISourceControlOperation::Create<FMarkForAdd>(), FilesToAdd, TEXT("mark for add"));
    }

    EWorkingCopyKind FWriteSourceControl::DetectWorkingCopy(const FString& StartDirectory, FString& OutRoot)
    {
        IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
        FString Current = FPaths::ConvertRelativePathToFull(StartDirectory);
        FPaths::NormalizeDirectoryName(Current);
        while (!Current.IsEmpty())
        {
            if (ExistsAsFileOrDirectory(PlatformFile, FPaths::Combine(Current, TEXT(".git"))))
            {
                OutRoot = Current;
                return EWorkingCopyKind::Git;
            }
            if (PlatformFile.DirectoryExists(*FPaths::Combine(Current, TEXT(".svn"))))
            {
                OutRoot = Current;
                return EWorkingCopyKind::Svn;
            }
            const FString Parent = FPaths::GetPath(Current);
            if (Parent.IsEmpty() || Parent == Current)
            {
                break;
            }
            Current = Parent;
        }
        OutRoot.Reset();
        return EWorkingCopyKind::None;
    }
}
