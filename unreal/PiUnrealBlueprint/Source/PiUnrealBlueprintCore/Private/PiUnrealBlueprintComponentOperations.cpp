#include "PiUnrealBlueprintComponentOperations.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "PiUnrealBlueprintTypeSystem.h"
#include "UObject/UnrealType.h"

namespace PiUnrealBlueprint
{
    namespace
    {
        FBlueprintOperationError ComponentError(const FString& Code, const FString& Message, UBlueprint* Blueprint,
            const int32 OperationIndex, const FString& Callsite)
        {
            FBlueprintOperationError Error;
            Error.Code = Code;
            Error.Message = Message;
            Error.AssetPath = Blueprint ? Blueprint->GetPathName() : FString();
            Error.OperationIndex = OperationIndex;
            Error.UECallsite = Callsite;
            return Error;
        }

        FBlueprintOperationResult ErrorResult(const FBlueprintOperationError& Error)
        {
            FBlueprintOperationResult Result;
            Result.Error = Error;
            return Result;
        }

        FBlueprintOperationResult ComponentWrongThread(const int32 OperationIndex, const FString& Callsite)
        {
            return ErrorResult(ComponentError(TEXT("COMPONENT_WRONG_THREAD"),
                TEXT("Blueprint component operations must run on the game thread."), nullptr, OperationIndex, Callsite));
        }

        UBlueprint* BlueprintFromClass(UClass* Class)
        {
            return Class ? Cast<UBlueprint>(Class->ClassGeneratedBy) : nullptr;
        }
    }

    USCS_Node* FBlueprintComponentOperations::ResolveLocalNode(UBlueprint* Blueprint,
        const FComponentReference& Component, FBlueprintOperationError& OutError, const int32 OperationIndex)
    {
        if (Blueprint == nullptr || Blueprint->SimpleConstructionScript == nullptr)
        {
            OutError = ComponentError(TEXT("COMPONENT_SCS_UNAVAILABLE"), TEXT("Blueprint has no SimpleConstructionScript."),
                Blueprint, OperationIndex, TEXT("FBlueprintComponentOperations::ResolveLocalNode"));
            return nullptr;
        }
        USCS_Node* Node = Component.NodeGuid.IsValid()
            ? Blueprint->SimpleConstructionScript->FindSCSNodeByGuid(Component.NodeGuid)
            : Blueprint->SimpleConstructionScript->FindSCSNode(Component.VariableName);
        if (Node == nullptr || (!Component.VariableName.IsNone() && Node->GetVariableName() != Component.VariableName))
        {
            OutError = ComponentError(TEXT("COMPONENT_NOT_FOUND"),
                FString::Printf(TEXT("Local component '%s' was not found."), *Component.VariableName.ToString()),
                Blueprint, OperationIndex, TEXT("FBlueprintComponentOperations::ResolveLocalNode"));
        }
        return Node;
    }

    USCS_Node* FBlueprintComponentOperations::ResolveNodeInHierarchy(UBlueprint* Blueprint,
        const FComponentReference& Component, UBlueprint*& OutOwnerBlueprint, FBlueprintOperationError& OutError,
        const int32 OperationIndex)
    {
        for (UBlueprint* Current = Blueprint; Current != nullptr; Current = BlueprintFromClass(Current->ParentClass))
        {
            if (!Component.OwnerBlueprintPath.IsEmpty() && Current->GetPathName() != Component.OwnerBlueprintPath)
            {
                continue;
            }
            if (Current->SimpleConstructionScript == nullptr)
            {
                continue;
            }
            USCS_Node* Node = Component.NodeGuid.IsValid()
                ? Current->SimpleConstructionScript->FindSCSNodeByGuid(Component.NodeGuid)
                : Current->SimpleConstructionScript->FindSCSNode(Component.VariableName);
            if (Node != nullptr && (Component.VariableName.IsNone() || Node->GetVariableName() == Component.VariableName))
            {
                OutOwnerBlueprint = Current;
                return Node;
            }
        }
        OutError = ComponentError(TEXT("COMPONENT_NOT_FOUND"),
            FString::Printf(TEXT("Component '%s' was not found in the Blueprint hierarchy."), *Component.VariableName.ToString()),
            Blueprint, OperationIndex, TEXT("FBlueprintComponentOperations::ResolveNodeInHierarchy"));
        return nullptr;
    }

    UActorComponent* FBlueprintComponentOperations::ResolveTemplate(UBlueprint* Blueprint,
        const FComponentReference& Component, const bool bCreateInheritedOverride, FBlueprintOperationError& OutError,
        const int32 OperationIndex)
    {
        UBlueprint* OwnerBlueprint = nullptr;
        USCS_Node* Node = ResolveNodeInHierarchy(Blueprint, Component, OwnerBlueprint, OutError, OperationIndex);
        if (Node == nullptr)
        {
            return nullptr;
        }
        if (OwnerBlueprint == Blueprint)
        {
            return Node->ComponentTemplate;
        }
        if (Blueprint->GeneratedClass == nullptr)
        {
            OutError = ComponentError(TEXT("COMPONENT_CLASS_UNAVAILABLE"), TEXT("Compile the child Blueprint before creating an inherited override."),
                Blueprint, OperationIndex, TEXT("FBlueprintComponentOperations::ResolveTemplate"));
            return nullptr;
        }
        UInheritableComponentHandler* Handler = Blueprint->GetInheritableComponentHandler(bCreateInheritedOverride);
        if (Handler == nullptr)
        {
            OutError = ComponentError(TEXT("COMPONENT_OVERRIDE_UNAVAILABLE"), TEXT("Inherited component handler is unavailable."),
                Blueprint, OperationIndex, TEXT("FBlueprintComponentOperations::ResolveTemplate"));
            return nullptr;
        }
        const FComponentKey Key(Node);
        UActorComponent* Template = Handler->GetOverridenComponentTemplate(Key);
        if (Template == nullptr && bCreateInheritedOverride)
        {
            Handler->Modify();
            Template = Handler->CreateOverridenComponentTemplate(Key);
        }
        if (Template == nullptr)
        {
            OutError = ComponentError(TEXT("COMPONENT_OVERRIDE_NOT_FOUND"), TEXT("The inherited component has no child override."),
                Blueprint, OperationIndex, TEXT("UInheritableComponentHandler::GetOverridenComponentTemplate"));
        }
        return Template;
    }

    TSharedRef<FJsonObject> FBlueprintComponentOperations::DescribeNode(UBlueprint* QueryBlueprint,
        UBlueprint* OwnerBlueprint, USCS_Node* Node, const bool bInherited, FBlueprintOperationError& OutError)
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("variableName"), Node->GetVariableName().ToString());
        Json->SetStringField(TEXT("nodeGuid"), Node->VariableGuid.ToString(EGuidFormats::DigitsWithHyphens));
        Json->SetStringField(TEXT("ownerBlueprintPath"), OwnerBlueprint->GetPathName());
        Json->SetBoolField(TEXT("inherited"), bInherited);
        Json->SetStringField(TEXT("componentClassPath"), Node->ComponentClass ? Node->ComponentClass->GetPathName() : FString());
        Json->SetStringField(TEXT("templatePath"), Node->ComponentTemplate ? Node->ComponentTemplate->GetPathName() : FString());
        if (USCS_Node* Parent = OwnerBlueprint->SimpleConstructionScript->FindParentNode(Node))
        {
            Json->SetStringField(TEXT("parentVariableName"), Parent->GetVariableName().ToString());
            Json->SetStringField(TEXT("parentNodeGuid"), Parent->VariableGuid.ToString(EGuidFormats::DigitsWithHyphens));
        }
        else if (!Node->ParentComponentOrVariableName.IsNone())
        {
            Json->SetStringField(TEXT("parentVariableName"), Node->ParentComponentOrVariableName.ToString());
            Json->SetStringField(TEXT("parentOwnerClassName"), Node->ParentComponentOwnerClassName.ToString());
        }
        Json->SetBoolField(TEXT("root"), Node->IsRootNode());

        UActorComponent* EffectiveTemplate = Node->ComponentTemplate;
        if (bInherited && QueryBlueprint->GeneratedClass)
        {
            EffectiveTemplate = Node->GetActualComponentTemplate(Cast<UBlueprintGeneratedClass>(QueryBlueprint->GeneratedClass));
        }
        if (const USceneComponent* Scene = Cast<USceneComponent>(EffectiveTemplate))
        {
            const FTransform Transform(Scene->GetRelativeRotation(), Scene->GetRelativeLocation(), Scene->GetRelativeScale3D());
            TSharedRef<FJsonObject> TransformJson = MakeShared<FJsonObject>();
            TransformJson->SetStringField(TEXT("location"), Transform.GetLocation().ToString());
            TransformJson->SetStringField(TEXT("rotation"), Transform.Rotator().ToString());
            TransformJson->SetStringField(TEXT("scale"), Transform.GetScale3D().ToString());
            Json->SetObjectField(TEXT("relativeTransform"), TransformJson);
        }
        return Json;
    }

    FBlueprintOperationResult FBlueprintComponentOperations::List(UBlueprint* Blueprint, const bool bIncludeInherited,
        const int32 OperationIndex)
    {
        if (!IsInGameThread()) return ComponentWrongThread(OperationIndex, TEXT("FBlueprintComponentOperations::List"));
        if (Blueprint == nullptr || Blueprint->SimpleConstructionScript == nullptr)
        {
            return FBlueprintOperationResult::Failure(TEXT("COMPONENT_SCS_UNAVAILABLE"), TEXT("Blueprint has no SimpleConstructionScript."),
                Blueprint ? Blueprint->GetPathName() : FString(), OperationIndex, TEXT("FBlueprintComponentOperations::List"));
        }
        FBlueprintOperationResult Result = FBlueprintOperationResult::Success({Blueprint->GetPathName()}, false);
        TArray<TSharedPtr<FJsonValue>> Components;
        for (UBlueprint* Current = Blueprint; Current != nullptr; Current = bIncludeInherited ? BlueprintFromClass(Current->ParentClass) : nullptr)
        {
            if (Current->SimpleConstructionScript == nullptr) continue;
            TArray<USCS_Node*> Nodes = Current->SimpleConstructionScript->GetAllNodes();
            for (USCS_Node* Node : Nodes)
            {
                FBlueprintOperationError Error;
                Components.Add(MakeShared<FJsonValueObject>(DescribeNode(Blueprint, Current, Node, Current != Blueprint, Error)));
            }
        }
        Result.Data->SetArrayField(TEXT("components"), Components);
        return Result;
    }

    FBlueprintOperationResult FBlueprintComponentOperations::Add(UBlueprint* Blueprint, UClass* ComponentClass,
        const FName VariableName, const TOptional<FComponentReference>& Parent, const FTransform& RelativeTransform,
        const int32 OperationIndex)
    {
        if (!IsInGameThread()) return ComponentWrongThread(OperationIndex, TEXT("FBlueprintComponentOperations::Add"));
        if (Blueprint == nullptr || Blueprint->SimpleConstructionScript == nullptr || ComponentClass == nullptr ||
            !ComponentClass->IsChildOf(UActorComponent::StaticClass()) || VariableName.IsNone())
        {
            return FBlueprintOperationResult::Failure(TEXT("COMPONENT_ADD_INVALID"), TEXT("Blueprint, component class, and variable name are required."),
                Blueprint ? Blueprint->GetPathName() : FString(), OperationIndex, TEXT("FBlueprintComponentOperations::Add"));
        }
        if (Blueprint->SimpleConstructionScript->FindSCSNode(VariableName) != nullptr)
        {
            return FBlueprintOperationResult::Failure(TEXT("COMPONENT_ALREADY_EXISTS"), FString::Printf(TEXT("Component '%s' already exists."), *VariableName.ToString()),
                Blueprint->GetPathName(), OperationIndex, TEXT("FBlueprintComponentOperations::Add"));
        }
        Blueprint->Modify();
        Blueprint->SimpleConstructionScript->Modify();
        USCS_Node* Node = Blueprint->SimpleConstructionScript->CreateNode(ComponentClass, VariableName);
        if (Node == nullptr || Node->ComponentTemplate == nullptr)
        {
            return FBlueprintOperationResult::Failure(TEXT("COMPONENT_CREATE_FAILED"), TEXT("USimpleConstructionScript failed to create the component node."),
                Blueprint->GetPathName(), OperationIndex, TEXT("USimpleConstructionScript::CreateNode"));
        }
        Node->Modify();
        Node->ComponentTemplate->Modify();
        if (USceneComponent* Scene = Cast<USceneComponent>(Node->ComponentTemplate))
        {
            Scene->SetRelativeLocation_Direct(RelativeTransform.GetLocation());
            Scene->SetRelativeRotation_Direct(RelativeTransform.Rotator());
            Scene->SetRelativeScale3D_Direct(RelativeTransform.GetScale3D());
        }
        Blueprint->SimpleConstructionScript->AddNode(Node);
        if (Parent.IsSet())
        {
            FBlueprintOperationResult AttachResult = Attach(Blueprint,
                FComponentReference{VariableName, Node->VariableGuid, Blueprint->GetPathName(), false}, Parent, OperationIndex);
            if (!AttachResult.bSuccess)
            {
                Blueprint->SimpleConstructionScript->RemoveNode(Node);
                return AttachResult;
            }
        }
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        FBlueprintOperationResult Result = FBlueprintOperationResult::Success({Blueprint->GetPathName()});
        Result.Data->SetStringField(TEXT("variableName"), VariableName.ToString());
        Result.Data->SetStringField(TEXT("nodeGuid"), Node->VariableGuid.ToString(EGuidFormats::DigitsWithHyphens));
        return Result;
    }

    FBlueprintOperationResult FBlueprintComponentOperations::Remove(UBlueprint* Blueprint,
        const FComponentReference& Component, const bool bPromoteChildren, const int32 OperationIndex)
    {
        if (!IsInGameThread()) return ComponentWrongThread(OperationIndex, TEXT("FBlueprintComponentOperations::Remove"));
        FBlueprintOperationError Error;
        USCS_Node* Node = ResolveLocalNode(Blueprint, Component, Error, OperationIndex);
        if (Node == nullptr) return ErrorResult(Error);
        Blueprint->Modify();
        Blueprint->SimpleConstructionScript->Modify();
        Node->Modify();
        if (bPromoteChildren) Blueprint->SimpleConstructionScript->RemoveNodeAndPromoteChildren(Node);
        else if (Node->GetChildNodes().Num() > 0)
        {
            return FBlueprintOperationResult::Failure(TEXT("COMPONENT_HAS_CHILDREN"), TEXT("Set promoteChildren=true or detach the child components first."),
                Blueprint->GetPathName(), OperationIndex, TEXT("FBlueprintComponentOperations::Remove"));
        }
        else Blueprint->SimpleConstructionScript->RemoveNode(Node);
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        return FBlueprintOperationResult::Success({Blueprint->GetPathName()});
    }

    FBlueprintOperationResult FBlueprintComponentOperations::Rename(UBlueprint* Blueprint,
        const FComponentReference& Component, const FName NewVariableName, const int32 OperationIndex)
    {
        if (!IsInGameThread()) return ComponentWrongThread(OperationIndex, TEXT("FBlueprintComponentOperations::Rename"));
        FBlueprintOperationError Error;
        USCS_Node* Node = ResolveLocalNode(Blueprint, Component, Error, OperationIndex);
        if (Node == nullptr) return ErrorResult(Error);
        if (NewVariableName.IsNone() || Blueprint->SimpleConstructionScript->FindSCSNode(NewVariableName) != nullptr)
        {
            return FBlueprintOperationResult::Failure(TEXT("COMPONENT_NAME_INVALID"), TEXT("The new component name is empty or already used."),
                Blueprint->GetPathName(), OperationIndex, TEXT("FBlueprintComponentOperations::Rename"));
        }
        Blueprint->Modify();
        Node->Modify();
        FBlueprintEditorUtils::RenameComponentMemberVariable(Blueprint, Node, NewVariableName);
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        return FBlueprintOperationResult::Success({Blueprint->GetPathName()});
    }

    FBlueprintOperationResult FBlueprintComponentOperations::Attach(UBlueprint* Blueprint,
        const FComponentReference& Component, const TOptional<FComponentReference>& NewParent, const int32 OperationIndex)
    {
        if (!IsInGameThread()) return ComponentWrongThread(OperationIndex, TEXT("FBlueprintComponentOperations::Attach"));
        FBlueprintOperationError Error;
        USCS_Node* Node = ResolveLocalNode(Blueprint, Component, Error, OperationIndex);
        if (Node == nullptr) return ErrorResult(Error);
        if (!Node->ComponentClass->IsChildOf(USceneComponent::StaticClass()))
        {
            return FBlueprintOperationResult::Failure(TEXT("COMPONENT_NOT_SCENE"), TEXT("Only scene components can be attached."),
                Blueprint->GetPathName(), OperationIndex, TEXT("FBlueprintComponentOperations::Attach"));
        }
        UBlueprint* ParentOwner = nullptr;
        USCS_Node* ParentNode = nullptr;
        if (NewParent.IsSet())
        {
            ParentNode = ResolveNodeInHierarchy(Blueprint, NewParent.GetValue(), ParentOwner, Error, OperationIndex);
            if (ParentNode == nullptr) return ErrorResult(Error);
            if (ParentNode == Node || ParentNode->IsChildOf(Node))
            {
                return FBlueprintOperationResult::Failure(TEXT("COMPONENT_ATTACHMENT_CYCLE"), TEXT("The requested attachment would create a cycle."),
                    Blueprint->GetPathName(), OperationIndex, TEXT("FBlueprintComponentOperations::Attach"));
            }
            if (!ParentNode->ComponentClass->IsChildOf(USceneComponent::StaticClass()))
            {
                return FBlueprintOperationResult::Failure(TEXT("COMPONENT_PARENT_NOT_SCENE"), TEXT("Attachment parent must be a scene component."),
                    Blueprint->GetPathName(), OperationIndex, TEXT("FBlueprintComponentOperations::Attach"));
            }
        }
        Blueprint->Modify();
        Blueprint->SimpleConstructionScript->Modify();
        Node->Modify();
        if (USCS_Node* OldParent = Blueprint->SimpleConstructionScript->FindParentNode(Node)) OldParent->RemoveChildNode(Node, false);
        else Blueprint->SimpleConstructionScript->RemoveNode(Node, false);
        if (ParentNode == nullptr)
        {
            Node->ParentComponentOrVariableName = NAME_None;
            Node->ParentComponentOwnerClassName = NAME_None;
            Blueprint->SimpleConstructionScript->AddNode(Node);
        }
        else if (ParentOwner == Blueprint)
        {
            ParentNode->Modify();
            ParentNode->AddChildNode(Node, true);
        }
        else
        {
            // UE4.27 distinguishes inherited SCS parents from native component parents by owner class name.
            Node->SetParent(ParentNode);
            Blueprint->SimpleConstructionScript->AddNode(Node);
        }
        Blueprint->SimpleConstructionScript->ValidateSceneRootNodes();
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        return FBlueprintOperationResult::Success({Blueprint->GetPathName()});
    }

    FBlueprintOperationResult FBlueprintComponentOperations::SetRoot(UBlueprint* Blueprint,
        const FComponentReference& Component, const int32 OperationIndex)
    {
        if (!IsInGameThread()) return ComponentWrongThread(OperationIndex, TEXT("FBlueprintComponentOperations::SetRoot"));
        FBlueprintOperationError Error;
        USCS_Node* Node = ResolveLocalNode(Blueprint, Component, Error, OperationIndex);
        if (Node == nullptr) return ErrorResult(Error);
        if (!Node->ComponentClass->IsChildOf(USceneComponent::StaticClass()))
        {
            return FBlueprintOperationResult::Failure(TEXT("COMPONENT_ROOT_NOT_SCENE"), TEXT("Root component must be a scene component."),
                Blueprint->GetPathName(), OperationIndex, TEXT("FBlueprintComponentOperations::SetRoot"));
        }
        const TArray<USCS_Node*> ExistingRoots = Blueprint->SimpleConstructionScript->GetRootNodes();
        FBlueprintOperationResult DetachResult = Attach(Blueprint, Component, TOptional<FComponentReference>(), OperationIndex);
        if (!DetachResult.bSuccess) return DetachResult;
        for (USCS_Node* Root : ExistingRoots)
        {
            if (Root == Node || !Root->ComponentClass->IsChildOf(USceneComponent::StaticClass())) continue;
            Blueprint->SimpleConstructionScript->RemoveNode(Root, false);
            Node->AddChildNode(Root, true);
        }
        Blueprint->SimpleConstructionScript->ValidateSceneRootNodes();
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        return FBlueprintOperationResult::Success({Blueprint->GetPathName()});
    }

    FBlueprintOperationResult FBlueprintComponentOperations::SetTransform(UBlueprint* Blueprint,
        const FComponentReference& Component, const FTransform& RelativeTransform, const int32 OperationIndex)
    {
        if (!IsInGameThread()) return ComponentWrongThread(OperationIndex, TEXT("FBlueprintComponentOperations::SetTransform"));
        FBlueprintOperationError Error;
        UActorComponent* Template = ResolveTemplate(Blueprint, Component, true, Error, OperationIndex);
        USceneComponent* Scene = Cast<USceneComponent>(Template);
        if (Scene == nullptr)
        {
            if (Template != nullptr) Error = ComponentError(TEXT("COMPONENT_NOT_SCENE"), TEXT("Only scene components have transforms."),
                Blueprint, OperationIndex, TEXT("FBlueprintComponentOperations::SetTransform"));
            return ErrorResult(Error);
        }
        Blueprint->Modify();
        Scene->Modify();
        Scene->SetRelativeLocation_Direct(RelativeTransform.GetLocation());
        Scene->SetRelativeRotation_Direct(RelativeTransform.Rotator());
        Scene->SetRelativeScale3D_Direct(RelativeTransform.GetScale3D());
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        return FBlueprintOperationResult::Success({Blueprint->GetPathName()});
    }

    FBlueprintOperationResult FBlueprintComponentOperations::SetProperty(UBlueprint* Blueprint,
        const FComponentReference& Component, const FString& PropertyPath, const TSharedPtr<FJsonValue>& Value,
        const bool bCreateInheritedOverride, const int32 OperationIndex)
    {
        if (!IsInGameThread()) return ComponentWrongThread(OperationIndex, TEXT("FBlueprintComponentOperations::SetProperty"));
        FBlueprintOperationError Error;
        UActorComponent* Template = ResolveTemplate(Blueprint, Component, bCreateInheritedOverride, Error, OperationIndex);
        if (Template == nullptr) return ErrorResult(Error);
        Blueprint->Modify();
        if (!FBlueprintTypeSystem::SetPropertyValue(Template, PropertyPath, Value, Error,
            Blueprint->GetPathName(), OperationIndex)) return ErrorResult(Error);
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        return FBlueprintOperationResult::Success({Blueprint->GetPathName()});
    }

    FBlueprintOperationResult FBlueprintComponentOperations::ClearInheritedOverride(UBlueprint* Blueprint,
        const FComponentReference& Component, const int32 OperationIndex)
    {
        if (!IsInGameThread()) return ComponentWrongThread(OperationIndex, TEXT("FBlueprintComponentOperations::ClearInheritedOverride"));
        FBlueprintOperationError Error;
        UBlueprint* OwnerBlueprint = nullptr;
        USCS_Node* Node = ResolveNodeInHierarchy(Blueprint, Component, OwnerBlueprint, Error, OperationIndex);
        if (Node == nullptr) return ErrorResult(Error);
        if (OwnerBlueprint == Blueprint)
        {
            return FBlueprintOperationResult::Failure(TEXT("COMPONENT_NOT_INHERITED"), TEXT("Local components do not have inherited overrides."),
                Blueprint->GetPathName(), OperationIndex, TEXT("FBlueprintComponentOperations::ClearInheritedOverride"));
        }
        UInheritableComponentHandler* Handler = Blueprint->GetInheritableComponentHandler(false);
        const FComponentKey Key(Node);
        if (Handler == nullptr || Handler->GetOverridenComponentTemplate(Key) == nullptr)
        {
            return FBlueprintOperationResult::Failure(TEXT("COMPONENT_OVERRIDE_NOT_FOUND"), TEXT("The inherited component has no child override."),
                Blueprint->GetPathName(), OperationIndex, TEXT("FBlueprintComponentOperations::ClearInheritedOverride"));
        }
        Blueprint->Modify();
        Handler->Modify();
        Handler->RemoveOverridenComponentTemplate(Key);
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        return FBlueprintOperationResult::Success({Blueprint->GetPathName()});
    }
}
