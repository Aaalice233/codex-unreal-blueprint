#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "CodexUnrealBlueprintWritePipeline.h"

class UWidgetBlueprint;

namespace CodexUnrealBlueprint
{
    struct CODEXUNREALBLUEPRINTCORE_API FUmgOperationError
    {
        FString Code;
        FString Message;
        FString AssetPath;
        FString WidgetPath;
        FString AnimationName;
        FString UECallsite;
        int32 OperationIndex = INDEX_NONE;
        TArray<FString> Details;

        bool IsSet() const { return !Code.IsEmpty(); }
        TSharedRef<FJsonObject> ToJson() const;
    };

    struct CODEXUNREALBLUEPRINTCORE_API FUmgOperationResult
    {
        bool bChanged = false;
        FString AssetPath;
        TArray<FString> ImpactPackages;
        TSharedPtr<FJsonObject> Data;

        TSharedRef<FJsonObject> ToJson() const;
    };

    /**
     * UE4.27 Widget Blueprint mutator and deterministic inspector. Mutations are
     * intentionally exposed through FWritePipeline so Modify/compile/save/reload
     * semantics have one implementation for the Editor transport and every MCP caller.
     *
     * Supported operations:
     * widget.add/remove/rename/reparent, namedSlot.set/clear,
     * slot.property.set, widget.property.set, widget.variable.set,
     * event.bind/unbind, binding.set/remove, navigation.set/clear, accessibility.set,
     * animation.add/remove/rename, animation.binding.add/remove,
     * animation.track.add/remove, animation.section.add/remove/set,
     * animation.key.add/update/remove.
     */
    class CODEXUNREALBLUEPRINTCORE_API FBlueprintUmgOperations
    {
    public:
        static bool Apply(UWidgetBlueprint* Blueprint, const TSharedRef<FJsonObject>& Operation,
            FWriteMutationContext& Context, FUmgOperationResult& OutResult, FUmgOperationError& OutError,
            int32 OperationIndex = INDEX_NONE);

        static bool Inspect(UWidgetBlueprint* Blueprint, TSharedRef<FJsonObject>& OutSnapshot,
            FUmgOperationError& OutError);
        static bool VerifySnapshot(UWidgetBlueprint* Blueprint, const TSharedRef<FJsonObject>& Expected,
            TSharedRef<FJsonObject>& OutActual, FUmgOperationError& OutError);

        static bool BuildWriteRequest(UWidgetBlueprint* Blueprint,
            const TArray<TSharedRef<FJsonObject>>& Operations, const FString& RequestId,
            const TFunction<bool(const FString&, FString&, FString&)>& StateHashResolver,
            FWritePipelineRequest& OutRequest, FUmgOperationError& OutError);

        static FWritePipelineResult Execute(UWidgetBlueprint* Blueprint,
            const TArray<TSharedRef<FJsonObject>>& Operations, const FString& RequestId,
            const TFunction<bool(const FString&, FString&, FString&)>& StateHashResolver,
            const FWritePipelineProgress& Progress, FUmgOperationError& OutError);

    private:
        FBlueprintUmgOperations() = delete;
    };
}
