#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "CodexUnrealBlueprintOperationResult.h"

class UActorComponent;
class UBlueprint;
class USCS_Node;

namespace CodexUnrealBlueprint
{
    struct CODEXUNREALBLUEPRINTCORE_API FComponentReference
    {
        FName VariableName;
        FGuid NodeGuid;
        FString OwnerBlueprintPath;
        bool bInherited = false;
    };

    class CODEXUNREALBLUEPRINTCORE_API FBlueprintComponentOperations
    {
    public:
        static FBlueprintOperationResult List(UBlueprint* Blueprint, bool bIncludeInherited,
            int32 OperationIndex = INDEX_NONE);
        static FBlueprintOperationResult List(UBlueprint* Blueprint, bool bIncludeInherited,
            const TSharedPtr<FJsonObject>& Query, int32 OperationIndex = INDEX_NONE);
        static FBlueprintOperationResult Add(UBlueprint* Blueprint, UClass* ComponentClass, const FName VariableName,
            const TOptional<FComponentReference>& Parent, const FTransform& RelativeTransform,
            int32 OperationIndex = INDEX_NONE);
        static FBlueprintOperationResult CloneRange(UBlueprint* Blueprint, const FComponentReference& Source,
            const FString& TargetPattern, int32 StartIndex, int32 EndIndex,
            const TSharedPtr<FJsonObject>& PropertyOverrides, int32 OperationIndex = INDEX_NONE);
        static FBlueprintOperationResult ValidateCloneRange(UBlueprint* Blueprint, const FComponentReference& Source,
            const FString& TargetPattern, int32 StartIndex, int32 EndIndex,
            const TSharedPtr<FJsonObject>& PropertyOverrides, int32 OperationIndex = INDEX_NONE);
        static FBlueprintOperationResult Remove(UBlueprint* Blueprint, const FComponentReference& Component,
            bool bPromoteChildren, int32 OperationIndex = INDEX_NONE);
        static FBlueprintOperationResult Rename(UBlueprint* Blueprint, const FComponentReference& Component,
            const FName NewVariableName, int32 OperationIndex = INDEX_NONE);
        static FBlueprintOperationResult Attach(UBlueprint* Blueprint, const FComponentReference& Component,
            const TOptional<FComponentReference>& NewParent, int32 OperationIndex = INDEX_NONE);
        static FBlueprintOperationResult SetRoot(UBlueprint* Blueprint, const FComponentReference& Component,
            int32 OperationIndex = INDEX_NONE);
        static FBlueprintOperationResult SetTransform(UBlueprint* Blueprint, const FComponentReference& Component,
            const FTransform& RelativeTransform, int32 OperationIndex = INDEX_NONE);
        static FBlueprintOperationResult SetProperty(UBlueprint* Blueprint, const FComponentReference& Component,
            const FString& PropertyPath, const TSharedPtr<FJsonValue>& Value, bool bCreateInheritedOverride,
            int32 OperationIndex = INDEX_NONE);
        static FBlueprintOperationResult ClearInheritedOverride(UBlueprint* Blueprint,
            const FComponentReference& Component, int32 OperationIndex = INDEX_NONE);

    private:
        static USCS_Node* ResolveLocalNode(UBlueprint* Blueprint, const FComponentReference& Component,
            FBlueprintOperationError& OutError, int32 OperationIndex);
        static USCS_Node* ResolveNodeInHierarchy(UBlueprint* Blueprint, const FComponentReference& Component,
            UBlueprint*& OutOwnerBlueprint, FBlueprintOperationError& OutError, int32 OperationIndex);
        static UActorComponent* ResolveTemplate(UBlueprint* Blueprint, const FComponentReference& Component,
            bool bCreateInheritedOverride, FBlueprintOperationError& OutError, int32 OperationIndex);
        static TSharedRef<FJsonObject> DescribeNode(UBlueprint* QueryBlueprint, UBlueprint* OwnerBlueprint,
            USCS_Node* Node, bool bInherited, const TSet<FString>& Fields, const TArray<FString>& PropertyPaths,
            FBlueprintOperationError& OutError);
    };
}
