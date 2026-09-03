#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "CodexUnrealBlueprintWritePipeline.h"

class UAnimBlueprint;

namespace CodexUnrealBlueprint
{
    struct CODEXUNREALBLUEPRINTCORE_API FAnimOperationError
    {
        FString Code;
        FString Message;
        FString AssetPath;
        FString GraphPath;
        FString NodeGuid;
        FString PinGuid;
        FString UECallsite;
        int32 OperationIndex = INDEX_NONE;
        TArray<FString> Candidates;

        bool IsSet() const { return !Code.IsEmpty(); }
        TSharedRef<FJsonObject> ToJson() const;
    };

    struct CODEXUNREALBLUEPRINTCORE_API FAnimOperationResult
    {
        bool bChanged = false;
        FString AssetPath;
        FString GraphPath;
        FString GraphGuid;
        TArray<FString> NodeGuids;
        TArray<FString> PinGuids;
        TArray<FString> ImpactPackages;
        TSharedPtr<FJsonObject> Data;

        TSharedRef<FJsonObject> ToJson() const;
    };

    /**
     * UE4.27 Animation Blueprint operations. AnimGraph nodes are resolved through
     * FBlueprintActionDatabase spawners; state-machine nodes are created through
     * UAnimationStateMachineSchema's schema action. Connections are always made
     * by the owning animation schema, never by K2 pin mutation.
     *
     * Supported operations:
     * anim.skeleton.set, anim.parent.set,
     * anim.node.spawn/delete/move/property.set,
     * anim.stateMachine.add/remove/rename/entry.set,
     * anim.state.add/remove/rename, anim.conduit.add/remove/rename,
     * anim.transition.add/remove/property.set,
     * anim.rule.node.spawn/delete/property.set, anim.rule.link.connect/disconnect,
     * anim.poseLink.connect/disconnect,
     * anim.variable.add/update/remove,
     * anim.event.node.spawn/delete/property.set, anim.event.link.connect/disconnect.
     */
    class CODEXUNREALBLUEPRINTCORE_API FBlueprintAnimOperations
    {
    public:
        static bool Apply(UAnimBlueprint* Blueprint, const TSharedRef<FJsonObject>& Operation,
            FWriteMutationContext& Context, FAnimOperationResult& OutResult, FAnimOperationError& OutError,
            int32 OperationIndex = INDEX_NONE);

        static bool Inspect(UAnimBlueprint* Blueprint, TSharedRef<FJsonObject>& OutSnapshot,
            FAnimOperationError& OutError);
        static bool VerifySnapshot(UAnimBlueprint* Blueprint, const TSharedRef<FJsonObject>& Expected,
            TSharedRef<FJsonObject>& OutActual, FAnimOperationError& OutError);

        static bool BuildWriteRequest(UAnimBlueprint* Blueprint,
            const TArray<TSharedRef<FJsonObject>>& Operations, const FString& RequestId,
            const TFunction<bool(const FString&, FString&, FString&)>& StateHashResolver,
            FWritePipelineRequest& OutRequest, FAnimOperationError& OutError);

        static FWritePipelineResult Execute(UAnimBlueprint* Blueprint,
            const TArray<TSharedRef<FJsonObject>>& Operations, const FString& RequestId,
            const TFunction<bool(const FString&, FString&, FString&)>& StateHashResolver,
            const FWritePipelineProgress& Progress, FAnimOperationError& OutError);

    private:
        FBlueprintAnimOperations() = delete;
    };
}
