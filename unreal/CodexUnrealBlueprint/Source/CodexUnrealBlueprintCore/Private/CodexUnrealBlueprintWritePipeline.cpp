#include "CodexUnrealBlueprintWritePipeline.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Async/Async.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/Blueprint.h"
#include "HAL/PlatformProcess.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "CodexUnrealBlueprintSourceControl.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectGlobals.h"

namespace CodexUnrealBlueprint
{
    namespace
    {
        TArray<TSharedPtr<FJsonValue>> WritePipelineStringsToJson(const TArray<FString>& Values)
        {
            TArray<TSharedPtr<FJsonValue>> Json;
            for (const FString& Value : Values)
            {
                Json.Add(MakeShared<FJsonValueString>(Value));
            }
            return Json;
        }

        bool EnterPhase(
            const FWritePipelineProgress& Progress,
            const FString& Phase,
            const bool bCancellationSafe,
            const FString& Message,
            FWritePipelineError& OutError)
        {
            if (bCancellationSafe && Progress.IsCancellationRequested && Progress.IsCancellationRequested())
            {
                OutError.Code = TEXT("write.cancelled");
                OutError.Message = FString::Printf(TEXT("Write cancelled before phase %s."), *Phase);
                OutError.UECallsite = TEXT("FWritePipeline::EnterPhase");
                return false;
            }
            if (Progress.EnterPhase && !Progress.EnterPhase(Phase, bCancellationSafe, Message))
            {
                OutError.Code = TEXT("write.phaseRejected");
                OutError.Message = FString::Printf(TEXT("The job manager rejected phase %s."), *Phase);
                OutError.UECallsite = TEXT("FWritePipeline::EnterPhase");
                return false;
            }
            if (Progress.Heartbeat)
            {
                Progress.Heartbeat();
            }
            return true;
        }

        void Report(
            const FWritePipelineProgress& Progress,
            const int32 Completed,
            const int32 Total,
            const FString& Message,
            const FString& AssetPath = FString())
        {
            if (Progress.Report)
            {
                Progress.Report(Completed, Total, Message, AssetPath);
            }
            if (Progress.Heartbeat)
            {
                Progress.Heartbeat();
            }
        }

        UObject* FindTopLevelAsset(UPackage* Package)
        {
            if (Package == nullptr)
            {
                return nullptr;
            }
            TArray<UObject*> Objects;
            GetObjectsWithOuter(Package, Objects, false, RF_Transient);
            for (UObject* Object : Objects)
            {
                if (Object != nullptr && Object->HasAnyFlags(RF_Standalone))
                {
                    return Object;
                }
            }
            return nullptr;
        }

        void AddCompilerMessages(const FCompilerResultsLog& Log, TArray<FString>& OutMessages)
        {
            for (const TSharedRef<FTokenizedMessage>& Message : Log.Messages)
            {
                OutMessages.Add(Message->ToText().ToString());
            }
        }

        FString ResolveHash(
            const FWritePipelineRequest& Request,
            const FString& PackageName,
            FString& OutError)
        {
            FString Hash;
            const bool bSucceeded = Request.StateHashResolver
                ? Request.StateHashResolver(PackageName, Hash, OutError)
                : FWritePreflight::ComputePackageStateHash(PackageName, Hash, OutError);
            return bSucceeded ? Hash : FString();
        }

        void PopulateFailureReport(
            FWritePipelineResult& Result,
            const FString& Phase,
            const FString& Message)
        {
            Result.FailureReport.bPartial = Result.bPartial;
            Result.FailureReport.bStateUnknown = Result.bStateUnknown;
            Result.FailureReport.FailedPhase = Phase;
            Result.FailureReport.Message = Message;
            Result.FailureReport.Assets.Reset();
            bool bAnySaveAttempted = false;
            for (const FWritePackageResult& Package : Result.Packages)
            {
                bAnySaveAttempted = bAnySaveAttempted || Package.bSaveAttempted;
            }
            for (const FWritePackageResult& Package : Result.Packages)
            {
                FWriteAssetFailureState Asset;
                Asset.PackageName = Package.PackageName;
                Asset.Filename = Package.Filename;
                Asset.BeforeHash = Package.BeforeHash;
                Asset.LastConfirmedHash = Package.SavedHash;
                Asset.bSaveAttempted = Package.bSaveAttempted;
                Asset.bSaveSucceeded = Package.bSaved;
                Asset.bReloadVerified = Package.bVerified;
                if (Package.bVerified)
                {
                    Asset.State = EWriteAssetState::Saved;
                }
                else if (Result.bStateUnknown && (Package.bSaveAttempted || !bAnySaveAttempted))
                {
                    Asset.State = EWriteAssetState::Unknown;
                }
                else if (Package.bSaved)
                {
                    Asset.State = EWriteAssetState::Saved;
                }
                else if (Package.bSaveAttempted || !bAnySaveAttempted)
                {
                    Asset.State = EWriteAssetState::NotSaved;
                }
                else
                {
                    Asset.State = EWriteAssetState::Modified;
                }
                Result.FailureReport.Assets.Add(MoveTemp(Asset));
            }
            Result.FailureReport.Finalize(FPaths::ProjectDir());
        }

        void SetError(
            FWritePipelineResult& Result,
            const FString& Code,
            const FString& Message,
            const FString& Callsite,
            const FString& AssetPath = FString(),
            const int32 OperationIndex = INDEX_NONE)
        {
            Result.Error.Code = Code;
            Result.Error.Message = Message;
            Result.Error.UECallsite = Callsite;
            Result.Error.AssetPath = AssetPath;
            Result.Error.OperationIndex = OperationIndex;
        }

    }

    bool FWriteMutationContext::Modify(UObject* Object, FWritePipelineError& OutError)
    {
        if (Object == nullptr)
        {
            OutError.Code = TEXT("write.modifyNull");
            OutError.Message = TEXT("An operation attempted to modify a null UObject.");
            OutError.UECallsite = TEXT("FWriteMutationContext::Modify");
            return false;
        }
        // Modify 的返回值只表示对象是否写入当前 undo buffer；Commandlet 没有 GUndo 时会返回 false，
        // 但对象仍可被合法修改，不能据此把真实 headless 写入误判为失败。
        Object->SetFlags(RF_Transactional);
        Object->Modify(true);
        ModifiedObjects.Add(Object);
        if (UPackage* Package = Object->GetOutermost())
        {
            ChangedPackages.Add(Package);
        }
        return true;
    }

    bool FWriteMutationContext::WasModified(const UObject* Object) const
    {
        return ModifiedObjects.Contains(Object);
    }

    void FWriteMutationContext::MarkPackageChanged(UPackage* Package)
    {
        if (Package != nullptr)
        {
            ChangedPackages.Add(Package);
            Package->SetDirtyFlag(true);
        }
    }

    const TSet<UPackage*>& FWriteMutationContext::GetChangedPackages() const
    {
        return ChangedPackages;
    }

    TSharedRef<FJsonObject> FWritePackageResult::ToJson() const
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("packageName"), PackageName);
        Json->SetStringField(TEXT("filename"), Filename);
        Json->SetStringField(TEXT("beforeHash"), BeforeHash);
        Json->SetStringField(TEXT("savedHash"), SavedHash);
        Json->SetBoolField(TEXT("saveAttempted"), bSaveAttempted);
        Json->SetBoolField(TEXT("saved"), bSaved);
        Json->SetBoolField(TEXT("markedForAdd"), bMarkedForAdd);
        Json->SetBoolField(TEXT("reloaded"), bReloaded);
        Json->SetBoolField(TEXT("verified"), bVerified);
        if (!Error.IsEmpty()) Json->SetStringField(TEXT("error"), Error);
        return Json;
    }

    TSharedRef<FJsonObject> FWritePipelineResult::ToJson() const
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetBoolField(TEXT("success"), bSucceeded);
        Json->SetBoolField(TEXT("partial"), bPartial);
        Json->SetBoolField(TEXT("stateUnknown"), bStateUnknown);
        Json->SetArrayField(TEXT("impactPackages"), WritePipelineStringsToJson(ImpactPackages));
        Json->SetArrayField(TEXT("compilerWarnings"), WritePipelineStringsToJson(CompilerWarnings));
        TArray<TSharedPtr<FJsonValue>> PackageJson;
        for (const FWritePackageResult& Package : Packages)
        {
            PackageJson.Add(MakeShared<FJsonValueObject>(Package.ToJson()));
        }
        Json->SetArrayField(TEXT("packages"), PackageJson);
        if (!Error.Code.IsEmpty())
        {
            TSharedRef<FJsonObject> ErrorJson = MakeShared<FJsonObject>();
            ErrorJson->SetStringField(TEXT("code"), Error.Code);
            ErrorJson->SetStringField(TEXT("message"), Error.Message);
            ErrorJson->SetStringField(TEXT("assetPath"), Error.AssetPath);
            ErrorJson->SetStringField(TEXT("ueCallsite"), Error.UECallsite);
            ErrorJson->SetNumberField(TEXT("operationIndex"), Error.OperationIndex);
            ErrorJson->SetArrayField(TEXT("compilerMessages"), WritePipelineStringsToJson(Error.CompilerMessages));
            Json->SetObjectField(TEXT("error"), ErrorJson);
            Json->SetObjectField(TEXT("failureReport"), FailureReport.ToJson());
        }
        return Json;
    }

    FWritePipelineResult FWritePipeline::Execute(const FWritePipelineRequest& Request, const FWritePipelineProgress& Progress)
    {
        if (IsInGameThread())
        {
            return ExecuteOnGameThread(Request, Progress);
        }

        FWritePipelineResult Result;
        FEvent* Completion = FPlatformProcess::GetSynchEventFromPool(true);
        AsyncTask(ENamedThreads::GameThread, [&Request, &Progress, &Result, Completion]()
        {
            Result = ExecuteOnGameThread(Request, Progress);
            Completion->Trigger();
        });
        while (!Completion->Wait(250))
        {
            if (Progress.Heartbeat)
            {
                Progress.Heartbeat();
            }
        }
        FPlatformProcess::ReturnSynchEventToPool(Completion);
        return Result;
    }

    FWritePipelineResult FWritePipeline::ExecuteOnGameThread(
        const FWritePipelineRequest& Request,
        const FWritePipelineProgress& Progress)
    {
        check(IsInGameThread());
        FWritePipelineResult Result;
        if (Request.RequestId.TrimStartAndEnd().IsEmpty())
        {
            SetError(Result, TEXT("write.requestIdRequired"), TEXT("A non-empty requestId is required."),
                TEXT("FWritePipeline::ExecuteOnGameThread"));
            PopulateFailureReport(Result, TEXT("preflight"), Result.Error.Message);
            return Result;
        }
        if (Request.Operations.Num() == 0)
        {
            SetError(Result, TEXT("write.operationsRequired"), TEXT("At least one write operation is required."),
                TEXT("FWritePipeline::ExecuteOnGameThread"));
            PopulateFailureReport(Result, TEXT("preflight"), Result.Error.Message);
            return Result;
        }

        FPreflightRequest PreflightRequest = Request.Preflight;
        PreflightRequest.StateHashResolver = Request.StateHashResolver;
        for (const TSharedRef<IWriteOperation>& Operation : Request.Operations)
        {
            Operation->GatherPreflight(PreflightRequest);
        }

        if (!EnterPhase(Progress, TEXT("preflight"), true, TEXT("Validating affected packages and references."), Result.Error))
        {
            PopulateFailureReport(Result, TEXT("preflight"), Result.Error.Message);
            return Result;
        }
        const FPreflightResult Preflight = FWritePreflight::Run(PreflightRequest);
        for (const FImpactPackage& Impact : Preflight.ImpactPackages)
        {
            Result.ImpactPackages.Add(Impact.PackageName);
            FWritePackageResult Package;
            Package.PackageName = Impact.PackageName;
            Package.Filename = Impact.Filename;
            Package.BeforeHash = Impact.BeforeHash;
            Result.Packages.Add(MoveTemp(Package));
        }
        if (!Preflight.bSucceeded)
        {
            const FPreflightIssue& Issue = Preflight.Issues[0];
            SetError(Result, Issue.Code, Issue.Message, TEXT("FWritePreflight::Run"), Issue.PackageName, Issue.OperationIndex);
            PopulateFailureReport(Result, TEXT("preflight"), Result.Error.Message);
            return Result;
        }

        TArray<FString> ExistingFiles;
        TArray<FString> NewFiles;
        for (const FImpactPackage& Impact : Preflight.ImpactPackages)
        {
            (Impact.bExistsOnDisk ? ExistingFiles : NewFiles).Add(Impact.Filename);
        }
        const FSourceControlResult Checkout = FWriteSourceControl::Checkout(ExistingFiles);
        if (!Checkout.bSucceeded)
        {
            SetError(Result, TEXT("write.checkoutFailed"), Checkout.Error, TEXT("FWriteSourceControl::Checkout"));
            PopulateFailureReport(Result, TEXT("preflight"), Result.Error.Message);
            return Result;
        }
        const FSourceControlResult PostCheckout = FWriteSourceControl::Inspect(ExistingFiles, NewFiles);
        if (!PostCheckout.bSucceeded)
        {
            SetError(Result, TEXT("write.checkoutVerifyFailed"), PostCheckout.Error, TEXT("FWriteSourceControl::Inspect"));
            PopulateFailureReport(Result, TEXT("preflight"), Result.Error.Message);
            return Result;
        }
        for (const FSourceControlFileState& File : PostCheckout.Files)
        {
            if (File.bExists && File.bReadOnly)
            {
                SetError(Result, TEXT("write.checkoutVerifyFailed"),
                    FString::Printf(TEXT("Affected file remains read-only after checkout: %s."), *File.Filename),
                    TEXT("FWriteSourceControl::Inspect"), File.Filename);
                PopulateFailureReport(Result, TEXT("preflight"), Result.Error.Message);
                return Result;
            }
        }

        if (!EnterPhase(Progress, TEXT("modify"), true, TEXT("Applying operations in an Unreal transaction."), Result.Error))
        {
            PopulateFailureReport(Result, TEXT("modify"), Result.Error.Message);
            return Result;
        }

        FWriteMutationContext MutationContext;
        bool bTransactionSucceeded = true;
        FString FailedPhase = TEXT("modify");
        {
            const FText Description = FText::FromString(Request.TransactionDescription.IsEmpty()
                ? FString::Printf(TEXT("Codex Blueprint write %s"), *Request.RequestId)
                : Request.TransactionDescription);
            FScopedTransaction Transaction(TEXT("CodexUnrealBlueprint.WritePipeline"), Description, nullptr, true);

            for (const FImpactPackage& Impact : Preflight.ImpactPackages)
            {
                UPackage* Package = FindPackage(nullptr, *Impact.PackageName);
                if (Package == nullptr)
                {
                    Package = Impact.bExistsOnDisk
                        ? LoadPackage(nullptr, *Impact.PackageName, LOAD_None)
                        : CreatePackage(*Impact.PackageName);
                }
                if (Package == nullptr || !MutationContext.Modify(Package, Result.Error))
                {
                    bTransactionSucceeded = false;
                    break;
                }
                TArray<UObject*> Objects;
                GetObjectsWithOuter(Package, Objects, true, RF_Transient);
                for (UObject* Object : Objects)
                {
                    if (!MutationContext.Modify(Object, Result.Error))
                    {
                        bTransactionSucceeded = false;
                        break;
                    }
                }
                if (!bTransactionSucceeded)
                {
                    break;
                }
            }

            for (int32 Index = 0; bTransactionSucceeded && Index < Request.Operations.Num(); ++Index)
            {
                const TSharedRef<IWriteOperation>& Operation = Request.Operations[Index];
                Report(Progress, Index, Request.Operations.Num(), TEXT("Applying operation."));
                if (!Operation->Apply(MutationContext, Result.Error))
                {
                    if (Result.Error.OperationIndex == INDEX_NONE)
                    {
                        Result.Error.OperationIndex = Operation->GetOperationIndex();
                    }
                    if (Result.Error.Code.IsEmpty())
                    {
                        SetError(Result, TEXT("write.operationFailed"), TEXT("A write operation failed without an error."),
                            TEXT("IWriteOperation::Apply"), FString(), Operation->GetOperationIndex());
                    }
                    bTransactionSucceeded = false;
                }
            }

            if (bTransactionSucceeded)
            {
                TSet<FString> DeclaredPackages;
                for (const FImpactPackage& Impact : Preflight.ImpactPackages) DeclaredPackages.Add(Impact.PackageName);
                for (UPackage* ChangedPackage : MutationContext.GetChangedPackages())
                {
                    if (ChangedPackage != nullptr && !DeclaredPackages.Contains(ChangedPackage->GetName()))
                    {
                        SetError(Result, TEXT("write.undeclaredImpactPackage"),
                            FString::Printf(TEXT("An operation modified package '%s' without declaring it during preflight."), *ChangedPackage->GetName()),
                            TEXT("FWriteMutationContext::GetChangedPackages"), ChangedPackage->GetName());
                        bTransactionSucceeded = false;
                        break;
                    }
                }
            }

            for (const TSharedRef<IWriteOperation>& Operation : Request.Operations)
            {
                if (bTransactionSucceeded && !Operation->VerifyInMemory(Result.Error))
                {
                    if (Result.Error.OperationIndex == INDEX_NONE)
                    {
                        Result.Error.OperationIndex = Operation->GetOperationIndex();
                    }
                    if (Result.Error.Code.IsEmpty())
                    {
                        SetError(Result, TEXT("write.inMemoryVerifyFailed"), TEXT("In-memory operation verification failed."),
                            TEXT("IWriteOperation::VerifyInMemory"), FString(), Operation->GetOperationIndex());
                    }
                    bTransactionSucceeded = false;
                }
            }

            if (bTransactionSucceeded)
            {
                FailedPhase = TEXT("compile");
                if (Progress.IsCancellationRequested && Progress.IsCancellationRequested())
                {
                    SetError(Result, TEXT("write.cancelled"), TEXT("Write cancelled before Blueprint compilation."),
                        TEXT("FWritePipeline::ExecuteOnGameThread"));
                    bTransactionSucceeded = false;
                }
                else if (!EnterPhase(Progress, TEXT("compile"), false, TEXT("Compiling affected Blueprints in dependency order."), Result.Error))
                {
                    bTransactionSucceeded = false;
                }
            }
            for (int32 CompileIndex = 0; bTransactionSucceeded && CompileIndex < Preflight.CompileOrder.Num(); ++CompileIndex)
            {
                const FString& PackageName = Preflight.CompileOrder[CompileIndex];
                UPackage* Package = FindPackage(nullptr, *PackageName);
                if (Package == nullptr)
                {
                    Package = LoadPackage(nullptr, *PackageName, LOAD_None);
                }
                if (Package == nullptr)
                {
                    SetError(Result, TEXT("write.compilePackageMissing"),
                        FString::Printf(TEXT("Compile package is not loaded: %s."), *PackageName),
                        TEXT("LoadPackage"), PackageName);
                    bTransactionSucceeded = false;
                    break;
                }
                TArray<UObject*> Objects;
                GetObjectsWithOuter(Package, Objects, true, RF_Transient);
                TArray<UBlueprint*> Blueprints;
                for (UObject* Object : Objects)
                {
                    if (UBlueprint* Blueprint = Cast<UBlueprint>(Object))
                    {
                        Blueprints.AddUnique(Blueprint);
                    }
                }
                for (UBlueprint* Blueprint : Blueprints)
                {
                    TArray<FString> Messages;
                    bool bCompileSucceeded = false;
                    if (Request.BlueprintCompiler)
                    {
                        bCompileSucceeded = Request.BlueprintCompiler(Blueprint, Messages);
                    }
                    else
                    {
                        FCompilerResultsLog CompilerLog;
                        CompilerLog.bSilentMode = true;
                        FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &CompilerLog);
                        AddCompilerMessages(CompilerLog, Messages);
                        if (CompilerLog.NumWarnings > 0)
                        {
                            for (const TSharedRef<FTokenizedMessage>& Message : CompilerLog.Messages)
                            {
                                if (Message->GetSeverity() == EMessageSeverity::Warning
                                    || Message->GetSeverity() == EMessageSeverity::PerformanceWarning)
                                {
                                    Result.CompilerWarnings.Add(Message->ToText().ToString());
                                }
                            }
                        }
                        bCompileSucceeded = CompilerLog.NumErrors == 0 && Blueprint->IsUpToDate();
                    }
                    if (!bCompileSucceeded)
                    {
                        SetError(Result, TEXT("write.compileFailed"),
                            FString::Printf(TEXT("Blueprint compilation failed: %s."), *Blueprint->GetPathName()),
                            TEXT("FKismetEditorUtilities::CompileBlueprint"), Blueprint->GetPathName());
                        Result.Error.CompilerMessages = MoveTemp(Messages);
                        bTransactionSucceeded = false;
                        break;
                    }
                }
                Report(Progress, CompileIndex + 1, Preflight.CompileOrder.Num(), TEXT("Compiled affected package."), PackageName);
            }
        }

        if (!bTransactionSucceeded)
        {
            if (GEditor == nullptr || !GEditor->UndoTransaction(false))
            {
                Result.bStateUnknown = true;
                if (Result.Error.Code.IsEmpty())
                {
                    SetError(Result, TEXT("write.undoFailed"), TEXT("The write transaction could not be undone."),
                        TEXT("UEditorEngine::UndoTransaction"));
                }
            }
            PopulateFailureReport(Result, FailedPhase, Result.Error.Message);
            return Result;
        }

        if (Progress.IsCancellationRequested && Progress.IsCancellationRequested())
        {
            SetError(Result, TEXT("write.cancelled"), TEXT("Write cancelled at the final pre-save safety point."),
                TEXT("FWritePipeline::ExecuteOnGameThread"));
            if (GEditor == nullptr || !GEditor->UndoTransaction(false))
            {
                Result.bStateUnknown = true;
                Result.Error.Code = TEXT("write.cancelUndoFailed");
                Result.Error.Message = TEXT("Cancellation was accepted, but the transaction could not be undone.");
                Result.Error.UECallsite = TEXT("UEditorEngine::UndoTransaction");
            }
            PopulateFailureReport(Result, TEXT("save"), Result.Error.Message);
            return Result;
        }
        if (!EnterPhase(Progress, TEXT("save"), false, TEXT("Saving affected packages one by one."), Result.Error))
        {
            Result.bStateUnknown = true;
            PopulateFailureReport(Result, TEXT("save"), Result.Error.Message);
            return Result;
        }
        bool bSaveFailure = false;
        int32 SavedCount = 0;
        for (int32 Index = 0; Index < Preflight.ImpactPackages.Num(); ++Index)
        {
            FWritePackageResult& PackageResult = Result.Packages[Index];
            if (bSaveFailure)
            {
                PackageResult.Error = TEXT("Not saved because an earlier package save failed.");
                continue;
            }
            UPackage* Package = FindPackage(nullptr, *PackageResult.PackageName);
            PackageResult.bSaveAttempted = true;
            if (Package == nullptr)
            {
                PackageResult.Error = TEXT("Affected package disappeared before save.");
                bSaveFailure = true;
            }
            else
            {
                UObject* Asset = FindTopLevelAsset(Package);
                PackageResult.bSaved = UPackage::SavePackage(
                    Package,
                    Asset,
                    RF_Standalone,
                    *PackageResult.Filename,
                    GError,
                    nullptr,
                    false,
                    true,
                    SAVE_NoError,
                    nullptr,
                    FDateTime::MinValue(),
                    true);
                if (!PackageResult.bSaved)
                {
                    PackageResult.Error = TEXT("UPackage::SavePackage returned false.");
                    bSaveFailure = true;
                }
                else
                {
                    ++SavedCount;
                    FString HashError;
                    PackageResult.SavedHash = ResolveHash(Request, PackageResult.PackageName, HashError);
                    if (PackageResult.SavedHash.IsEmpty())
                    {
                        PackageResult.Error = HashError;
                        bSaveFailure = true;
                    }
                }
            }
            Report(Progress, Index + 1, Preflight.ImpactPackages.Num(), TEXT("Processed package save."), PackageResult.PackageName);
        }
        if (bSaveFailure)
        {
            Result.bPartial = SavedCount > 0;
            // A failed SavePackage call can leave the target file state indeterminate even when no prior package saved.
            Result.bStateUnknown = true;
            SetError(Result, TEXT("write.saveFailed"), TEXT("One or more packages failed to save; no automatic restore was attempted."),
                TEXT("UPackage::SavePackage"));
            PopulateFailureReport(Result, TEXT("save"), Result.Error.Message);
            return Result;
        }

        TArray<FString> SavedNewFiles;
        for (int32 Index = 0; Index < Preflight.ImpactPackages.Num(); ++Index)
        {
            if (!Preflight.ImpactPackages[Index].bExistsOnDisk)
            {
                SavedNewFiles.Add(Result.Packages[Index].Filename);
            }
        }
        const FSourceControlResult MarkForAdd = FWriteSourceControl::MarkForAdd(SavedNewFiles);
        if (!MarkForAdd.bSucceeded)
        {
            Result.bPartial = true;
            SetError(Result, TEXT("write.markForAddFailed"), MarkForAdd.Error, TEXT("FWriteSourceControl::MarkForAdd"));
            PopulateFailureReport(Result, TEXT("save"), Result.Error.Message);
            return Result;
        }
        for (int32 Index = 0; Index < Preflight.ImpactPackages.Num(); ++Index)
        {
            Result.Packages[Index].bMarkedForAdd = Preflight.ImpactPackages[Index].bExistsOnDisk
                || !MarkForAdd.bProviderEnabled
                || MarkForAdd.bSucceeded;
        }

        if (!EnterPhase(Progress, TEXT("reload"), false, TEXT("Reloading saved packages from disk."), Result.Error))
        {
            Result.bStateUnknown = true;
            PopulateFailureReport(Result, TEXT("reload"), Result.Error.Message);
            return Result;
        }
        TArray<UPackage*> PackagesToReload;
        for (int32 Index = 0; Index < Result.Packages.Num(); ++Index)
        {
            const FWritePackageResult& PackageResult = Result.Packages[Index];
            if (UPackage* Package = FindPackage(nullptr, *PackageResult.PackageName))
            {
                // UE4.27 的 FiB 缓存不会在新建 Blueprint 的热重载中自动移除旧对象，
                // 先同步 Asset Registry，避免新对象加载时覆盖仍有效的旧缓存条目。
                if (!Preflight.ImpactPackages[Index].bExistsOnDisk)
                {
                    TArray<UObject*> Assets;
                    GetObjectsWithOuter(Package, Assets, false, RF_Transient);
                    for (UObject* Asset : Assets)
                    {
                        if (Asset && Asset->IsAsset()) FAssetRegistryModule::AssetDeleted(Asset);
                    }
                }
                PackagesToReload.Add(Package);
            }
        }
        FText ReloadError;
        if (PackagesToReload.Num() != Result.Packages.Num()
            || !UPackageTools::ReloadPackages(PackagesToReload, ReloadError, EReloadPackagesInteractionMode::AssumePositive))
        {
            Result.bStateUnknown = true;
            SetError(Result, TEXT("write.reloadFailed"),
                ReloadError.IsEmpty() ? TEXT("One or more saved packages could not be reloaded.") : ReloadError.ToString(),
                TEXT("UPackageTools::ReloadPackages"));
            PopulateFailureReport(Result, TEXT("reload"), Result.Error.Message);
            return Result;
        }
        for (int32 Index = 0; Index < Result.Packages.Num(); ++Index)
        {
            FWritePackageResult& PackageResult = Result.Packages[Index];
            PackageResult.bReloaded = true;
            if (!Preflight.ImpactPackages[Index].bExistsOnDisk)
            {
                if (UPackage* ReloadedPackage = FindPackage(nullptr, *PackageResult.PackageName))
                {
                    TArray<UObject*> Assets;
                    GetObjectsWithOuter(ReloadedPackage, Assets, false, RF_Transient);
                    for (UObject* Asset : Assets)
                    {
                        if (Asset && Asset->IsAsset()) FAssetRegistryModule::AssetCreated(Asset);
                    }
                }
            }
        }

        if (!EnterPhase(Progress, TEXT("verify"), false, TEXT("Verifying reloaded package hashes and compile state."), Result.Error))
        {
            Result.bStateUnknown = true;
            PopulateFailureReport(Result, TEXT("verify"), Result.Error.Message);
            return Result;
        }
        bool bVerifyFailure = false;
        for (int32 Index = 0; Index < Result.Packages.Num(); ++Index)
        {
            FWritePackageResult& PackageResult = Result.Packages[Index];
            UPackage* Package = FindPackage(nullptr, *PackageResult.PackageName);
            FString HashError;
            const FString ReloadedHash = ResolveHash(Request, PackageResult.PackageName, HashError);
            bool bBlueprintsValid = Package != nullptr && !Package->IsDirty();
            if (Package != nullptr)
            {
                TArray<UObject*> Objects;
                GetObjectsWithOuter(Package, Objects, true, RF_Transient);
                for (UObject* Object : Objects)
                {
                    if (const UBlueprint* Blueprint = Cast<UBlueprint>(Object))
                    {
                        bBlueprintsValid = bBlueprintsValid && Blueprint->IsUpToDate();
                    }
                }
            }
            PackageResult.bVerified = bBlueprintsValid
                && !ReloadedHash.IsEmpty()
                && ReloadedHash.Equals(PackageResult.SavedHash, ESearchCase::IgnoreCase);
            if (!PackageResult.bVerified)
            {
                bVerifyFailure = true;
                PackageResult.Error = HashError.IsEmpty()
                    ? FString::Printf(TEXT("Reload verification failed (saved hash %s, reloaded hash %s, clean=%s)."),
                        *PackageResult.SavedHash, *ReloadedHash, bBlueprintsValid ? TEXT("true") : TEXT("false"))
                    : HashError;
            }
            Report(Progress, Index + 1, Result.Packages.Num(), TEXT("Verified reloaded package."), PackageResult.PackageName);
        }
        if (bVerifyFailure)
        {
            Result.bStateUnknown = true;
            SetError(Result, TEXT("write.reloadVerifyFailed"),
                TEXT("At least one saved package did not match its confirmed saved state after reload."),
                TEXT("FWritePipeline::VerifyReload"));
            PopulateFailureReport(Result, TEXT("verify"), Result.Error.Message);
            return Result;
        }

        Result.bSucceeded = true;
        return Result;
    }
}
