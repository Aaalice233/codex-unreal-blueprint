#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "PiUnrealBlueprintFailureReport.h"
#include "PiUnrealBlueprintPreflight.h"

class UBlueprint;
class UObject;
class UPackage;

namespace PiUnrealBlueprint
{
    struct PIUNREALBLUEPRINTCORE_API FWritePipelineError
    {
        FString Code;
        FString Message;
        FString AssetPath;
        FString UECallsite;
        int32 OperationIndex = INDEX_NONE;
        TArray<FString> CompilerMessages;
    };

    struct PIUNREALBLUEPRINTCORE_API FWritePipelineProgress
    {
        TFunction<bool(const FString& Phase, bool bCancellationSafe, const FString& Message)> EnterPhase;
        TFunction<void(int32 Completed, int32 Total, const FString& Message, const FString& AssetPath)> Report;
        TFunction<bool()> IsCancellationRequested;
        TFunction<void()> Heartbeat;
    };

    class PIUNREALBLUEPRINTCORE_API FWriteMutationContext
    {
    public:
        bool Modify(UObject* Object, FWritePipelineError& OutError);
        bool WasModified(const UObject* Object) const;
        void MarkPackageChanged(UPackage* Package);
        const TSet<UPackage*>& GetChangedPackages() const;

    private:
        TSet<const UObject*> ModifiedObjects;
        TSet<UPackage*> ChangedPackages;
    };

    class PIUNREALBLUEPRINTCORE_API IWriteOperation
    {
    public:
        virtual ~IWriteOperation() = default;
        virtual int32 GetOperationIndex() const = 0;
        virtual void GatherPreflight(FPreflightRequest& InOutRequest) const = 0;
        virtual bool Apply(FWriteMutationContext& Context, FWritePipelineError& OutError) = 0;
        virtual bool VerifyInMemory(FWritePipelineError& OutError) const = 0;
    };

    struct PIUNREALBLUEPRINTCORE_API FWritePackageResult
    {
        FString PackageName;
        FString Filename;
        FString BeforeHash;
        FString SavedHash;
        bool bSaveAttempted = false;
        bool bSaved = false;
        bool bMarkedForAdd = false;
        bool bReloaded = false;
        bool bVerified = false;
        FString Error;

        TSharedRef<FJsonObject> ToJson() const;
    };

    struct PIUNREALBLUEPRINTCORE_API FWritePipelineRequest
    {
        FString RequestId;
        FString TransactionDescription;
        FPreflightRequest Preflight;
        TArray<TSharedRef<IWriteOperation>> Operations;
        TFunction<bool(UBlueprint* Blueprint, TArray<FString>& OutMessages)> BlueprintCompiler;
        TFunction<bool(const FString& PackageName, FString& OutHash, FString& OutError)> StateHashResolver;
    };

    struct PIUNREALBLUEPRINTCORE_API FWritePipelineResult
    {
        bool bSucceeded = false;
        bool bPartial = false;
        bool bStateUnknown = false;
        TArray<FString> ImpactPackages;
        TArray<FString> CompilerWarnings;
        TArray<FWritePackageResult> Packages;
        FWritePipelineError Error;
        FWriteFailureReport FailureReport;

        TSharedRef<FJsonObject> ToJson() const;
    };

    class PIUNREALBLUEPRINTCORE_API FWritePipeline
    {
    public:
        static FWritePipelineResult Execute(const FWritePipelineRequest& Request, const FWritePipelineProgress& Progress);

    private:
        static FWritePipelineResult ExecuteOnGameThread(const FWritePipelineRequest& Request, const FWritePipelineProgress& Progress);
    };
}
