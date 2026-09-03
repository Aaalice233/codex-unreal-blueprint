#pragma once

#include "CoreMinimal.h"
#include "CodexUnrealBlueprintFailureReport.h"

namespace CodexUnrealBlueprint
{
    struct CODEXUNREALBLUEPRINTCORE_API FSourceControlFileState
    {
        FString Filename;
        bool bExists = false;
        bool bReadOnly = false;
        bool bSourceControlled = false;
        bool bCheckedOut = false;
        bool bAdded = false;
        bool bNeedsCheckout = false;
        bool bCanCheckout = false;
        bool bCanAdd = false;
        FString CheckedOutBy;
    };

    struct CODEXUNREALBLUEPRINTCORE_API FSourceControlResult
    {
        bool bSucceeded = false;
        bool bProviderEnabled = false;
        bool bProviderAvailable = false;
        FString ProviderName;
        FString Error;
        TArray<FSourceControlFileState> Files;
    };

    class CODEXUNREALBLUEPRINTCORE_API FWriteSourceControl
    {
    public:
        static FSourceControlResult Inspect(const TArray<FString>& ExistingFiles, const TArray<FString>& NewFiles);
        static FSourceControlResult Checkout(const TArray<FString>& ExistingFiles);
        static FSourceControlResult MarkForAdd(const TArray<FString>& SavedNewFiles);
        static EWorkingCopyKind DetectWorkingCopy(const FString& StartDirectory, FString& OutRoot);

    private:
        static FSourceControlResult Query(const TArray<FString>& ExistingFiles, const TArray<FString>& NewFiles);
    };
}
