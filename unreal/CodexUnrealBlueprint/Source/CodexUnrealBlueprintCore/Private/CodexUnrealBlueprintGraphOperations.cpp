#include "CodexUnrealBlueprintGraphOperations.h"
#include "CodexUnrealBlueprintTypeSystem.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphNode_Comment.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/LevelScriptBlueprint.h"
#include "GameFramework/Actor.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_EditablePinBase.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Knot.h"
#include "K2Node_Tunnel.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/UObjectGlobals.h"

namespace CodexUnrealBlueprint
{
    namespace
    {
        void Fail(FGraphActionError& Error, const TCHAR* Code, const FString& Message, const TCHAR* Callsite,
            UBlueprint* Blueprint = nullptr, UEdGraph* Graph = nullptr)
        {
            Error.Code = Code;
            Error.Message = Message;
            Error.UECallsite = Callsite;
            Error.AssetPath = Blueprint ? Blueprint->GetPathName() : FString();
            Error.GraphPath = Graph ? Graph->GetPathName() : FString();
        }

        bool ReadRequiredString(const FJsonObject& Json, const TCHAR* Field, FString& Out, FGraphActionError& Error, const TCHAR* Callsite)
        {
            if (!Json.TryGetStringField(Field, Out) || Out.TrimStartAndEnd().IsEmpty())
            {
                Fail(Error, TEXT("GraphOperationInvalidArgument"), FString::Printf(TEXT("'%s' must be a non-empty string."), Field), Callsite);
                return false;
            }
            return true;
        }

        bool ReadGuid(const FJsonObject& Json, const TCHAR* Field, FGuid& Out, FGraphActionError& Error, const TCHAR* Callsite)
        {
            FString Value;
            if (!ReadRequiredString(Json, Field, Value, Error, Callsite) || !FGuid::Parse(Value, Out) || !Out.IsValid())
            {
                if (!Error.IsSet()) Fail(Error, TEXT("GraphOperationInvalidGuid"), FString::Printf(TEXT("'%s' is not a valid GUID."), Field), Callsite);
                else Error.Code = TEXT("GraphOperationInvalidGuid");
                return false;
            }
            return true;
        }

        UEdGraph* FindGraph(UBlueprint* Blueprint, const FJsonObject& Json, FGraphActionError& Error)
        {
            FString GraphGuidText;
            FString GraphName;
            const bool bHasGuid = Json.TryGetStringField(TEXT("graphGuid"), GraphGuidText);
            const bool bHasName = Json.TryGetStringField(TEXT("graphName"), GraphName);
            if (bHasGuid == bHasName)
            {
                Fail(Error, TEXT("GraphReferenceInvalid"), TEXT("Specify exactly one of graphGuid or graphName."), TEXT("FindGraph"), Blueprint);
                return nullptr;
            }
            FGuid GraphGuid;
            if (bHasGuid && (!FGuid::Parse(GraphGuidText, GraphGuid) || !GraphGuid.IsValid()))
            {
                Fail(Error, TEXT("GraphReferenceInvalid"), TEXT("graphGuid is not a valid GUID."), TEXT("FindGraph"), Blueprint);
                return nullptr;
            }
            TArray<UEdGraph*> Graphs;
            Blueprint->GetAllGraphs(Graphs);
            TArray<UEdGraph*> Matches;
            for (UEdGraph* Graph : Graphs)
            {
                if (Graph && ((bHasGuid && Graph->GraphGuid == GraphGuid) || (bHasName && Graph->GetName() == GraphName))) Matches.Add(Graph);
            }
            if (Matches.Num() == 0)
            {
                Fail(Error, TEXT("GraphNotFound"), TEXT("The exact graph reference was not found."), TEXT("FindGraph"), Blueprint);
                return nullptr;
            }
            if (Matches.Num() > 1)
            {
                Fail(Error, TEXT("GraphReferenceAmbiguous"), TEXT("graphName matches multiple graphs; use graphGuid."), TEXT("FindGraph"), Blueprint);
                for (const UEdGraph* Match : Matches) Error.Candidates.Add(Match->GraphGuid.ToString(EGuidFormats::DigitsWithHyphens));
                return nullptr;
            }
            return Matches[0];
        }

        UEdGraphNode* FindNode(UEdGraph* Graph, const FGuid& Guid, FGraphActionError& Error)
        {
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node && Node->NodeGuid == Guid) return Node;
            }
            Fail(Error, TEXT("GraphNodeNotFound"), FString::Printf(TEXT("Node '%s' was not found."), *Guid.ToString()), TEXT("FindNode"), nullptr, Graph);
            return nullptr;
        }

        UEdGraphPin* FindPin(UEdGraph* Graph, const FJsonObject& Json, const TCHAR* Prefix, FGraphActionError& Error)
        {
            const FString NodeField = FString(Prefix) + TEXT("NodeGuid");
            const FString PinField = FString(Prefix) + TEXT("PinGuid");
            FGuid NodeGuid;
            FGuid PinGuid;
            if (!ReadGuid(Json, *NodeField, NodeGuid, Error, TEXT("FindPin")) || !ReadGuid(Json, *PinField, PinGuid, Error, TEXT("FindPin"))) return nullptr;
            UEdGraphNode* Node = FindNode(Graph, NodeGuid, Error);
            if (!Node) return nullptr;
            UEdGraphPin* Pin = Node->FindPinById(PinGuid);
            if (!Pin)
            {
                Fail(Error, TEXT("GraphPinNotFound"), FString::Printf(TEXT("Pin '%s' was not found on node '%s'."), *PinGuid.ToString(), *NodeGuid.ToString()), TEXT("FindPin"), nullptr, Graph);
            }
            return Pin;
        }

        bool ParsePinType(const FJsonObject& Json, FEdGraphPinType& Out, FGraphActionError& Error)
        {
            return FBlueprintTypeSystem::ParsePinType(MakeShared<FJsonObject>(Json), Out, Error,
                Error.AssetPath, Error.OperationIndex);
        }

        void FillResult(UBlueprint* Blueprint, UEdGraph* Graph, FGraphOperationResult& Result)
        {
            Result.bChanged = true;
            Result.AssetPath = Blueprint->GetPathName();
            if (Graph)
            {
                Result.GraphPath = Graph->GetPathName();
                Result.GraphGuid = Graph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphens);
            }
        }

        bool EnsureK2Graph(UBlueprint* Blueprint, UEdGraph* Graph, FGraphActionError& Error)
        {
            if (!Graph || !Cast<UEdGraphSchema_K2>(Graph->GetSchema()))
            {
                Fail(Error, TEXT("GraphSchemaUnsupported"), TEXT("This operation requires an exact UE4.27 K2 Schema graph."), TEXT("EnsureK2Graph"), Blueprint, Graph);
                return false;
            }
            return true;
        }

        bool AddGraph(UBlueprint* Blueprint, const FJsonObject& Op, FGraphOperationResult& Result, FGraphActionError& Error)
        {
            FString Kind;
            FString Name;
            if (!ReadRequiredString(Op, TEXT("kind"), Kind, Error, TEXT("AddGraph"))
                || !ReadRequiredString(Op, TEXT("name"), Name, Error, TEXT("AddGraph"))) return false;
            if (Name != FName(*Name).ToString())
            {
                Fail(Error, TEXT("GraphNameInvalid"), TEXT("name must be a valid, canonical FName."), TEXT("AddGraph"), Blueprint);
                return false;
            }
            TArray<UEdGraph*> Existing;
            Blueprint->GetAllGraphs(Existing);
            for (UEdGraph* Graph : Existing)
            {
                if (Graph && Graph->GetName() == Name)
                {
                    Fail(Error, TEXT("GraphNameConflict"), TEXT("A graph with this exact name already exists."), TEXT("AddGraph"), Blueprint, Graph);
                    return false;
                }
            }

            const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
            UEdGraph* Graph = nullptr;
            Blueprint->Modify();
            if (Kind == TEXT("function") || Kind == TEXT("interface"))
            {
                if (!Blueprint->SupportsFunctions())
                {
                    Fail(Error, TEXT("GraphKindIncompatible"), TEXT("This Blueprint type does not support function graphs."), TEXT("AddGraph"), Blueprint);
                    return false;
                }
                if (Kind == TEXT("interface") && Blueprint->BlueprintType != BPTYPE_Interface)
                {
                    Fail(Error, TEXT("GraphKindIncompatible"), TEXT("interface graphs can only be added to a Blueprint Interface."), TEXT("AddGraph"), Blueprint);
                    return false;
                }
                Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(*Name), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
                FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Graph, true, nullptr);
            }
            else if (Kind == TEXT("macro"))
            {
                if (!Blueprint->SupportsMacros())
                {
                    Fail(Error, TEXT("GraphKindIncompatible"), TEXT("This Blueprint type does not support macro graphs."), TEXT("AddGraph"), Blueprint);
                    return false;
                }
                Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(*Name), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
                FBlueprintEditorUtils::AddMacroGraph(Blueprint, Graph, true, nullptr);
            }
            else if (Kind == TEXT("event"))
            {
                if (Blueprint->BlueprintType == BPTYPE_Interface)
                {
                    Fail(Error, TEXT("GraphKindIncompatible"), TEXT("Blueprint Interfaces do not support event graphs."), TEXT("AddGraph"), Blueprint);
                    return false;
                }
                Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(*Name), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
                Schema->CreateDefaultNodesForGraph(*Graph);
                FBlueprintEditorUtils::AddUbergraphPage(Blueprint, Graph);
            }
            else if (Kind == TEXT("construction"))
            {
                if (!FBlueprintEditorUtils::IsActorBased(Blueprint) || FBlueprintEditorUtils::FindUserConstructionScript(Blueprint))
                {
                    Fail(Error, TEXT("GraphKindIncompatible"), TEXT("A Construction Script requires an Actor Blueprint without an existing Construction Script."), TEXT("AddGraph"), Blueprint);
                    return false;
                }
                if (Name != UEdGraphSchema_K2::FN_UserConstructionScript.ToString())
                {
                    Fail(Error, TEXT("GraphNameInvalid"), TEXT("A Construction Script must use the UE schema's canonical name."), TEXT("AddGraph"), Blueprint);
                    return false;
                }
                Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, UEdGraphSchema_K2::FN_UserConstructionScript, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
                FBlueprintEditorUtils::AddFunctionGraph(Blueprint, Graph, false, AActor::StaticClass());
                Graph->bAllowDeletion = false;
            }
            else if (Kind == TEXT("levelScript"))
            {
                if (!Cast<ULevelScriptBlueprint>(Blueprint))
                {
                    Fail(Error, TEXT("GraphKindIncompatible"), TEXT("levelScript graphs require a ULevelScriptBlueprint."), TEXT("AddGraph"), Blueprint);
                    return false;
                }
                Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(*Name), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
                Schema->CreateDefaultNodesForGraph(*Graph);
                FBlueprintEditorUtils::AddUbergraphPage(Blueprint, Graph);
            }
            else
            {
                Fail(Error, TEXT("GraphKindUnknown"), TEXT("kind must be event, construction, function, macro, interface, or levelScript."), TEXT("AddGraph"), Blueprint);
                return false;
            }
            if (!Graph)
            {
                Fail(Error, TEXT("GraphCreateFailed"), TEXT("UE4.27 failed to create the requested graph."), TEXT("FBlueprintEditorUtils::CreateNewGraph"), Blueprint);
                return false;
            }
            if (!Graph->GraphGuid.IsValid()) Graph->GraphGuid = FGuid::NewGuid();
            FillResult(Blueprint, Graph, Result);
            return true;
        }

        bool AddDispatcher(UBlueprint* Blueprint, const FJsonObject& Op, FGraphOperationResult& Result, FGraphActionError& Error)
        {
            FString Name;
            if (!ReadRequiredString(Op, TEXT("name"), Name, Error, TEXT("AddDispatcher"))) return false;
            if (!Blueprint->SupportsDelegates())
            {
                Fail(Error, TEXT("GraphKindIncompatible"), TEXT("This Blueprint type does not support Event Dispatchers."), TEXT("AddDispatcher"), Blueprint);
                return false;
            }
            const FName DispatcherName(*Name);
            if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, DispatcherName) != INDEX_NONE)
            {
                Fail(Error, TEXT("DispatcherNameConflict"), TEXT("A member with this dispatcher name already exists."), TEXT("AddDispatcher"), Blueprint);
                return false;
            }
            Blueprint->Modify();
            FEdGraphPinType Type;
            Type.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
            if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, DispatcherName, Type))
            {
                Fail(Error, TEXT("DispatcherCreateFailed"), TEXT("UE4.27 rejected the Event Dispatcher member."), TEXT("FBlueprintEditorUtils::AddMemberVariable"), Blueprint);
                return false;
            }
            UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, DispatcherName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
            if (!Graph)
            {
                FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, DispatcherName);
                Fail(Error, TEXT("DispatcherCreateFailed"), TEXT("UE4.27 failed to create the dispatcher signature graph."), TEXT("FBlueprintEditorUtils::CreateNewGraph"), Blueprint);
                return false;
            }
            const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
            Graph->bEditable = false;
            Schema->CreateDefaultNodesForGraph(*Graph);
            Schema->CreateFunctionGraphTerminators(*Graph, static_cast<UClass*>(nullptr));
            Schema->AddExtraFunctionFlags(Graph, FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public);
            Schema->MarkFunctionEntryAsEditable(Graph, true);
            Blueprint->DelegateSignatureGraphs.Add(Graph);
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
            FillResult(Blueprint, Graph, Result);
            return true;
        }

        bool SetSignature(UBlueprint* Blueprint, UEdGraph* Graph, const FJsonObject& Op, FGraphOperationResult& Result, FGraphActionError& Error)
        {
            if (!EnsureK2Graph(Blueprint, Graph, Error)) return false;
            TArray<UK2Node_EditablePinBase*> Inputs;
            TArray<UK2Node_EditablePinBase*> Outputs;
            FGuid RequestedSignatureNode;
            FString SignatureNodeText;
            const bool bTargetsNode = Op.TryGetStringField(TEXT("signatureNodeGuid"), SignatureNodeText);
            if (bTargetsNode && (!FGuid::Parse(SignatureNodeText, RequestedSignatureNode) || !RequestedSignatureNode.IsValid()))
            {
                Fail(Error, TEXT("GraphOperationInvalidGuid"), TEXT("signatureNodeGuid is not a valid GUID."), TEXT("SetSignature"), Blueprint, Graph);
                return false;
            }
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (bTargetsNode && Node->NodeGuid != RequestedSignatureNode) continue;
                if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node)) Inputs.Add(Entry);
                else if (UK2Node_CustomEvent* Event = Cast<UK2Node_CustomEvent>(Node)) Inputs.Add(Event);
                else if (UK2Node_FunctionResult* Return = Cast<UK2Node_FunctionResult>(Node)) Outputs.Add(Return);
                else if (UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(Node))
                {
                    bool bHasOutput = false;
                    for (const UEdGraphPin* Pin : Tunnel->Pins) if (Pin && Pin->Direction == EGPD_Output) bHasOutput = true;
                    (bHasOutput ? Inputs : Outputs).Add(Tunnel);
                }
            }
            if (Inputs.Num() != 1 || Outputs.Num() > 1)
            {
                Fail(Error, TEXT("GraphSignatureAmbiguous"), TEXT("The graph must have exactly one entry/event and at most one result terminator."), TEXT("SetSignature"), Blueprint, Graph);
                return false;
            }

            auto ReplacePins = [&](UK2Node_EditablePinBase* Node, const TArray<TSharedPtr<FJsonValue>>& Specs, EEdGraphPinDirection Direction) -> bool
            {
                Node->Modify();
                while (Node->UserDefinedPins.Num()) Node->RemoveUserDefinedPin(Node->UserDefinedPins.Last());
                for (const TSharedPtr<FJsonValue>& Value : Specs)
                {
                    const TSharedPtr<FJsonObject>* Spec = nullptr;
                    if (!Value.IsValid() || !Value->TryGetObject(Spec) || !Spec)
                    {
                        Fail(Error, TEXT("GraphSignatureInvalid"), TEXT("Each signature parameter must be an object."), TEXT("SetSignature"), Blueprint, Graph);
                        return false;
                    }
                    FString Name;
                    const TSharedPtr<FJsonObject>* TypeJson = nullptr;
                    if (!ReadRequiredString(**Spec, TEXT("name"), Name, Error, TEXT("SetSignature"))
                        || !(**Spec).TryGetObjectField(TEXT("type"), TypeJson) || !TypeJson)
                    {
                        if (!Error.IsSet()) Fail(Error, TEXT("GraphSignatureInvalid"), TEXT("Each parameter requires name and type."), TEXT("SetSignature"), Blueprint, Graph);
                        return false;
                    }
                    FEdGraphPinType Type;
                    if (!ParsePinType(**TypeJson, Type, Error)) return false;
                    FText Reason;
                    if (!Node->CanCreateUserDefinedPin(Type, Direction, Reason))
                    {
                        Fail(Error, TEXT("GraphSignaturePinRejected"), Reason.ToString(), TEXT("UK2Node_EditablePinBase::CanCreateUserDefinedPin"), Blueprint, Graph);
                        return false;
                    }
                    UEdGraphPin* Pin = Node->CreateUserDefinedPin(FName(*Name), Type, Direction, false);
                    if (!Pin)
                    {
                        Fail(Error, TEXT("GraphSignaturePinCreateFailed"), FString::Printf(TEXT("UE4.27 failed to create signature pin '%s'."), *Name), TEXT("UK2Node_EditablePinBase::CreateUserDefinedPin"), Blueprint, Graph);
                        return false;
                    }
                    if (!Pin->PersistentGuid.IsValid()) Pin->PersistentGuid = FGuid::NewGuid();
                    FString DefaultValue;
                    if ((**Spec).TryGetStringField(TEXT("defaultValue"), DefaultValue)) Pin->DefaultValue = DefaultValue;
                    Result.PinGuids.Add(Pin->PinId.ToString(EGuidFormats::DigitsWithHyphens));
                }
                return true;
            };

            const TArray<TSharedPtr<FJsonValue>>* InputSpecs = nullptr;
            const TArray<TSharedPtr<FJsonValue>>* OutputSpecs = nullptr;
            static const TArray<TSharedPtr<FJsonValue>> Empty;
            if (Op.HasField(TEXT("inputs")) && !Op.TryGetArrayField(TEXT("inputs"), InputSpecs))
            {
                Fail(Error, TEXT("GraphSignatureInvalid"), TEXT("inputs must be an array."), TEXT("SetSignature"), Blueprint, Graph);
                return false;
            }
            if (Op.HasField(TEXT("outputs")) && !Op.TryGetArrayField(TEXT("outputs"), OutputSpecs))
            {
                Fail(Error, TEXT("GraphSignatureInvalid"), TEXT("outputs must be an array."), TEXT("SetSignature"), Blueprint, Graph);
                return false;
            }
            if (!ReplacePins(Inputs[0], InputSpecs ? *InputSpecs : Empty, EGPD_Output)) return false;
            if ((OutputSpecs ? OutputSpecs->Num() : 0) > 0 && Outputs.Num() == 0)
            {
                GetDefault<UEdGraphSchema_K2>()->CreateFunctionGraphTerminators(*Graph, static_cast<UClass*>(nullptr));
                for (UEdGraphNode* Node : Graph->Nodes) if (UK2Node_FunctionResult* Return = Cast<UK2Node_FunctionResult>(Node)) Outputs.AddUnique(Return);
            }
            if (Outputs.Num() && !ReplacePins(Outputs[0], OutputSpecs ? *OutputSpecs : Empty, EGPD_Input)) return false;
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
            FillResult(Blueprint, Graph, Result);
            return true;
        }

        bool AddLocal(UBlueprint* Blueprint, UEdGraph* Graph, const FJsonObject& Op, FGraphOperationResult& Result, FGraphActionError& Error)
        {
            if (!EnsureK2Graph(Blueprint, Graph, Error) || !Blueprint->FunctionGraphs.Contains(Graph))
            {
                if (!Error.IsSet()) Fail(Error, TEXT("LocalVariableScopeInvalid"), TEXT("Local variables are only valid in an owned function graph."), TEXT("AddLocal"), Blueprint, Graph);
                return false;
            }
            FString Name;
            const TSharedPtr<FJsonObject>* TypeJson = nullptr;
            if (!ReadRequiredString(Op, TEXT("name"), Name, Error, TEXT("AddLocal")) || !Op.TryGetObjectField(TEXT("type"), TypeJson) || !TypeJson)
            {
                if (!Error.IsSet()) Fail(Error, TEXT("LocalVariableInvalid"), TEXT("local.add requires name and type."), TEXT("AddLocal"), Blueprint, Graph);
                return false;
            }
            FEdGraphPinType Type;
            if (!ParsePinType(**TypeJson, Type, Error)) return false;
            FString DefaultValue;
            Op.TryGetStringField(TEXT("defaultValue"), DefaultValue);
            Blueprint->Modify();
            Graph->Modify();
            if (!FBlueprintEditorUtils::AddLocalVariable(Blueprint, Graph, FName(*Name), Type, DefaultValue))
            {
                Fail(Error, TEXT("LocalVariableCreateFailed"), TEXT("UE4.27 rejected the local variable name, type, or scope."), TEXT("FBlueprintEditorUtils::AddLocalVariable"), Blueprint, Graph);
                return false;
            }
            FillResult(Blueprint, Graph, Result);
            return true;
        }

        bool SpawnNode(UBlueprint* Blueprint, UEdGraph* Graph, const FJsonObject& Op, FGraphOperationResult& Result, FGraphActionError& Error)
        {
            if (!EnsureK2Graph(Blueprint, Graph, Error)) return false;
            FString ActionId;
            if (!ReadRequiredString(Op, TEXT("actionId"), ActionId, Error, TEXT("SpawnNode"))) return false;
            double X = 0.0, Y = 0.0;
            if (!Op.TryGetNumberField(TEXT("x"), X) || !Op.TryGetNumberField(TEXT("y"), Y))
            {
                Fail(Error, TEXT("GraphNodeLocationRequired"), TEXT("node.spawn requires numeric x and y."), TEXT("SpawnNode"), Blueprint, Graph);
                return false;
            }
            UEdGraphNode* Node = nullptr;
            if (!FBlueprintGraphActionCatalog::Spawn(ActionId, Blueprint, Graph, FVector2D(X, Y), Node, Error,
                Error.OperationIndex)) return false;
            FString CustomEventName;
            if (Op.TryGetStringField(TEXT("customEventName"), CustomEventName))
            {
                UK2Node_CustomEvent* Event = Cast<UK2Node_CustomEvent>(Node);
                if (!Event || CustomEventName.IsEmpty())
                {
                    Node->DestroyNode();
                    Fail(Error, TEXT("GraphActionResultMismatch"), TEXT("customEventName is only valid for an action that spawned UK2Node_CustomEvent."), TEXT("SpawnNode"), Blueprint, Graph);
                    return false;
                }
                Event->CustomFunctionName = FName(*CustomEventName);
                Event->ReconstructNode();
            }
            FillResult(Blueprint, Graph, Result);
            Result.NodeGuids.Add(Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin) continue;
                if (!Pin->PersistentGuid.IsValid()) Pin->PersistentGuid = FGuid::NewGuid();
                Result.PinGuids.Add(Pin->PinId.ToString(EGuidFormats::DigitsWithHyphens));
            }
            FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
            return true;
        }

        bool CopyNodes(UBlueprint* Blueprint, UEdGraph* Graph, const FJsonObject& Op, FGraphOperationResult& Result, FGraphActionError& Error)
        {
            const TArray<TSharedPtr<FJsonValue>>* GuidValues = nullptr;
            if (!Op.TryGetArrayField(TEXT("nodeGuids"), GuidValues) || !GuidValues || GuidValues->Num() == 0)
            {
                Fail(Error, TEXT("GraphNodeSelectionRequired"), TEXT("node.copy requires a non-empty nodeGuids array."), TEXT("CopyNodes"), Blueprint, Graph);
                return false;
            }
            double OffsetX = 320.0, OffsetY = 0.0;
            Op.TryGetNumberField(TEXT("offsetX"), OffsetX);
            Op.TryGetNumberField(TEXT("offsetY"), OffsetY);
            TArray<UEdGraphNode*> Sources;
            for (const TSharedPtr<FJsonValue>& Value : *GuidValues)
            {
                FGuid Guid;
                FString GuidText;
                if (!Value.IsValid() || !Value->TryGetString(GuidText) || !FGuid::Parse(GuidText, Guid) || !Guid.IsValid())
                {
                    Fail(Error, TEXT("GraphOperationInvalidGuid"), TEXT("nodeGuids contains an invalid GUID."), TEXT("CopyNodes"), Blueprint, Graph);
                    return false;
                }
                UEdGraphNode* Node = FindNode(Graph, Guid, Error);
                if (!Node) return false;
                if (!Node->CanDuplicateNode() || !Node->CanPasteHere(Graph))
                {
                    Fail(Error, TEXT("GraphNodeNotDuplicable"), FString::Printf(TEXT("Node '%s' cannot be duplicated in this graph."), *Guid.ToString()), TEXT("UEdGraphNode::CanDuplicateNode/CanPasteHere"), Blueprint, Graph);
                    return false;
                }
                Sources.Add(Node);
            }
            Graph->Modify();
            TMap<UEdGraphPin*, UEdGraphPin*> Pins;
            for (UEdGraphNode* Source : Sources)
            {
                UEdGraphNode* Copy = DuplicateObject<UEdGraphNode>(Source, Graph);
                Copy->CreateNewGuid();
                Copy->NodePosX = Source->NodePosX + FMath::RoundToInt(OffsetX);
                Copy->NodePosY = Source->NodePosY + FMath::RoundToInt(OffsetY);
                for (int32 Index = 0; Index < Copy->Pins.Num(); ++Index)
                {
                    UEdGraphPin* CopyPin = Copy->Pins[Index];
                    UEdGraphPin* SourcePin = Source->Pins.IsValidIndex(Index) ? Source->Pins[Index] : nullptr;
                    if (CopyPin)
                    {
                        // DuplicateObject copies one-sided link pointers; resetting them must not mutate source nodes.
                        CopyPin->LinkedTo.Reset();
                        CopyPin->PinId = FGuid::NewGuid();
                        CopyPin->PersistentGuid = FGuid::NewGuid();
                        if (SourcePin) Pins.Add(SourcePin, CopyPin);
                    }
                }
                Graph->AddNode(Copy, true, false);
                Copy->PostPasteNode();
                Result.NodeGuids.Add(Copy->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
            }
            for (const TPair<UEdGraphPin*, UEdGraphPin*>& Pair : Pins)
            {
                for (UEdGraphPin* Linked : Pair.Key->LinkedTo)
                {
                    if (UEdGraphPin** CopyLinked = Pins.Find(Linked)) Pair.Value->MakeLinkTo(*CopyLinked);
                }
            }
            FillResult(Blueprint, Graph, Result);
            FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
            Graph->NotifyGraphChanged();
            return true;
        }

        bool CreatePrimitive(UBlueprint* Blueprint, UEdGraph* Graph, const FJsonObject& Op, bool bComment,
            FGraphOperationResult& Result, FGraphActionError& Error)
        {
            if (!EnsureK2Graph(Blueprint, Graph, Error)) return false;
            double X = 0.0, Y = 0.0;
            if (!Op.TryGetNumberField(TEXT("x"), X) || !Op.TryGetNumberField(TEXT("y"), Y))
            {
                Fail(Error, TEXT("GraphNodeLocationRequired"), TEXT("The operation requires numeric x and y."), TEXT("CreatePrimitive"), Blueprint, Graph);
                return false;
            }
            Graph->Modify();
            UEdGraphNode* Node = nullptr;
            if (bComment)
            {
                UEdGraphNode_Comment* Comment = NewObject<UEdGraphNode_Comment>(Graph, NAME_None, RF_Transactional);
                FString Text;
                if (!Op.TryGetStringField(TEXT("text"), Text)) Text = TEXT("Comment");
                double Width = 400.0, Height = 200.0;
                Op.TryGetNumberField(TEXT("width"), Width);
                Op.TryGetNumberField(TEXT("height"), Height);
                if (Width <= 0 || Height <= 0)
                {
                    Fail(Error, TEXT("GraphCommentSizeInvalid"), TEXT("Comment width and height must be positive."), TEXT("CreatePrimitive"), Blueprint, Graph);
                    return false;
                }
                Comment->NodeComment = Text;
                Comment->NodeWidth = Width;
                Comment->NodeHeight = Height;
                Node = Comment;
            }
            else
            {
                Node = NewObject<UK2Node_Knot>(Graph, NAME_None, RF_Transactional);
            }
            Node->CreateNewGuid();
            Node->NodePosX = FMath::RoundToInt(X);
            Node->NodePosY = FMath::RoundToInt(Y);
            Graph->AddNode(Node, true, false);
            Node->PostPlacedNewNode();
            if (UK2Node_Knot* Knot = Cast<UK2Node_Knot>(Node)) Knot->AllocateDefaultPins();
            FillResult(Blueprint, Graph, Result);
            Result.NodeGuids.Add(Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin) continue;
                if (!Pin->PersistentGuid.IsValid()) Pin->PersistentGuid = FGuid::NewGuid();
                Result.PinGuids.Add(Pin->PinId.ToString(EGuidFormats::DigitsWithHyphens));
            }
            FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
            return true;
        }

        bool LayoutGraph(UBlueprint* Blueprint, UEdGraph* Graph, const FJsonObject& Op, FGraphOperationResult& Result, FGraphActionError& Error)
        {
            TSet<UEdGraphNode*> Selection;
            const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
            if (Op.TryGetArrayField(TEXT("nodeGuids"), Values) && Values)
            {
                for (const TSharedPtr<FJsonValue>& Value : *Values)
                {
                    FGuid Guid;
                    FString GuidText;
                    if (!Value.IsValid() || !Value->TryGetString(GuidText) || !FGuid::Parse(GuidText, Guid) || !Guid.IsValid())
                    {
                        Fail(Error, TEXT("GraphOperationInvalidGuid"), TEXT("nodeGuids contains an invalid GUID."), TEXT("LayoutGraph"), Blueprint, Graph);
                        return false;
                    }
                    UEdGraphNode* Node = FindNode(Graph, Guid, Error);
                    if (!Node) return false;
                    Selection.Add(Node);
                }
            }
            else
            {
                for (UEdGraphNode* Node : Graph->Nodes) if (Node) Selection.Add(Node);
            }
            if (!Selection.Num())
            {
                Fail(Error, TEXT("GraphNodeSelectionRequired"), TEXT("No nodes are available for layout."), TEXT("LayoutGraph"), Blueprint, Graph);
                return false;
            }
            TArray<UEdGraphNode*> Ordered = Selection.Array();
            Ordered.Sort([](const UEdGraphNode& A, const UEdGraphNode& B) { return A.NodeGuid < B.NodeGuid; });
            TMap<UEdGraphNode*, int32> Columns;
            for (UEdGraphNode* Node : Ordered) Columns.Add(Node, 0);
            for (int32 Pass = 0; Pass < Ordered.Num(); ++Pass)
            {
                bool bChanged = false;
                for (UEdGraphNode* Node : Ordered)
                {
                    for (UEdGraphPin* Pin : Node->Pins)
                    {
                        if (!Pin || Pin->Direction != EGPD_Output) continue;
                        for (UEdGraphPin* Linked : Pin->LinkedTo)
                        {
                            UEdGraphNode* Target = Linked ? Linked->GetOwningNode() : nullptr;
                            if (Target && Selection.Contains(Target) && Columns[Target] < Columns[Node] + 1 && Columns[Node] + 1 < Ordered.Num())
                            {
                                Columns[Target] = Columns[Node] + 1;
                                bChanged = true;
                            }
                        }
                    }
                }
                if (!bChanged) break;
            }
            TMap<int32, int32> Rows;
            Graph->Modify();
            for (UEdGraphNode* Node : Ordered)
            {
                Node->Modify();
                const int32 Column = Columns[Node];
                const int32 Row = Rows.FindOrAdd(Column)++;
                Node->NodePosX = Column * 384;
                Node->NodePosY = Row * 192;
                Result.NodeGuids.Add(Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
            }
            Graph->NotifyGraphChanged();
            FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
            FillResult(Blueprint, Graph, Result);
            return true;
        }
    }

    TSharedRef<FJsonObject> FGraphOperationResult::ToJson() const
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetBoolField(TEXT("changed"), bChanged);
        Json->SetStringField(TEXT("assetPath"), AssetPath);
        if (!GraphPath.IsEmpty()) Json->SetStringField(TEXT("graphPath"), GraphPath);
        if (!GraphGuid.IsEmpty()) Json->SetStringField(TEXT("graphGuid"), GraphGuid);
        TArray<TSharedPtr<FJsonValue>> Nodes;
        for (const FString& Guid : NodeGuids) Nodes.Add(MakeShared<FJsonValueString>(Guid));
        Json->SetArrayField(TEXT("nodeGuids"), Nodes);
        TArray<TSharedPtr<FJsonValue>> Pins;
        for (const FString& Guid : PinGuids) Pins.Add(MakeShared<FJsonValueString>(Guid));
        Json->SetArrayField(TEXT("pinGuids"), Pins);
        if (Details.IsValid()) Json->SetObjectField(TEXT("details"), Details.ToSharedRef());
        return Json;
    }

    bool FBlueprintGraphOperations::Apply(UBlueprint* Blueprint, const TSharedRef<FJsonObject>& Operation,
        FGraphOperationResult& OutResult, FGraphActionError& OutError, const int32 OperationIndex)
    {
        OutResult = FGraphOperationResult();
        OutError = FGraphActionError();
        OutError.OperationIndex = OperationIndex;
        if (!IsInGameThread())
        {
            Fail(OutError, TEXT("GraphOperationWrongThread"), TEXT("Graph mutations must run on the game thread."), TEXT("FBlueprintGraphOperations::Apply"), Blueprint);
            return false;
        }
        if (!Blueprint)
        {
            Fail(OutError, TEXT("BlueprintRequired"), TEXT("A loaded UBlueprint is required."), TEXT("FBlueprintGraphOperations::Apply"));
            return false;
        }
        FString Name;
        if (!ReadRequiredString(*Operation, TEXT("operation"), Name, OutError, TEXT("FBlueprintGraphOperations::Apply"))) return false;
        if (Name == TEXT("graph.add")) return AddGraph(Blueprint, *Operation, OutResult, OutError);
        if (Name == TEXT("dispatcher.add")) return AddDispatcher(Blueprint, *Operation, OutResult, OutError);

        UEdGraph* Graph = nullptr;
        if (Name != TEXT("dispatcher.remove"))
        {
            Graph = FindGraph(Blueprint, *Operation, OutError);
            if (!Graph) return false;
        }
        if (Name == TEXT("graph.remove"))
        {
            if (!Graph->bAllowDeletion)
            {
                Fail(OutError, TEXT("GraphDeletionForbidden"), TEXT("UE marks this graph as non-deletable."), TEXT("UEdGraph::bAllowDeletion"), Blueprint, Graph);
                return false;
            }
            FillResult(Blueprint, Graph, OutResult);
            Blueprint->Modify();
            Graph->Modify();
            FBlueprintEditorUtils::RemoveGraph(Blueprint, Graph);
            return true;
        }
        if (Name == TEXT("signature.set")) return SetSignature(Blueprint, Graph, *Operation, OutResult, OutError);
        if (Name == TEXT("local.add")) return AddLocal(Blueprint, Graph, *Operation, OutResult, OutError);
        if (Name == TEXT("local.remove"))
        {
            FString LocalName;
            if (!ReadRequiredString(*Operation, TEXT("name"), LocalName, OutError, TEXT("local.remove"))) return false;
            if (!FBlueprintEditorUtils::FindLocalVariable(Blueprint, Graph, FName(*LocalName)))
            {
                Fail(OutError, TEXT("LocalVariableNotFound"), TEXT("The exact local variable was not found in this function graph."), TEXT("local.remove"), Blueprint, Graph);
                return false;
            }
            TArray<UK2Node_FunctionEntry*> Entries;
            Graph->GetNodesOfClass(Entries);
            if (Entries.Num() != 1)
            {
                Fail(OutError, TEXT("LocalVariableScopeInvalid"), TEXT("The function graph does not have exactly one entry node."), TEXT("local.remove"), Blueprint, Graph);
                return false;
            }
            Entries[0]->Modify();
            Entries[0]->LocalVariables.RemoveAll([&LocalName](const FBPVariableDescription& Variable)
            {
                return Variable.VarName == FName(*LocalName);
            });
            FBlueprintEditorUtils::RemoveVariableNodes(Blueprint, FName(*LocalName), true, Graph);
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
            FillResult(Blueprint, Graph, OutResult);
            return true;
        }
        if (Name == TEXT("dispatcher.remove"))
        {
            FString DispatcherName;
            if (!ReadRequiredString(*Operation, TEXT("name"), DispatcherName, OutError, TEXT("dispatcher.remove"))) return false;
            UEdGraph* Signature = FBlueprintEditorUtils::GetDelegateSignatureGraphByName(Blueprint, FName(*DispatcherName));
            if (!Signature || FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, FName(*DispatcherName)) == INDEX_NONE)
            {
                Fail(OutError, TEXT("DispatcherNotFound"), TEXT("The exact Event Dispatcher was not found."), TEXT("dispatcher.remove"), Blueprint);
                return false;
            }
            FillResult(Blueprint, Signature, OutResult);
            Blueprint->Modify();
            FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, FName(*DispatcherName));
            FBlueprintEditorUtils::RemoveGraph(Blueprint, Signature);
            return true;
        }
        if (Name == TEXT("node.spawn")) return SpawnNode(Blueprint, Graph, *Operation, OutResult, OutError);
        if (Name == TEXT("node.copy")) return CopyNodes(Blueprint, Graph, *Operation, OutResult, OutError);
        if (Name == TEXT("node.comment")) return CreatePrimitive(Blueprint, Graph, *Operation, true, OutResult, OutError);
        if (Name == TEXT("node.reroute")) return CreatePrimitive(Blueprint, Graph, *Operation, false, OutResult, OutError);
        if (Name == TEXT("graph.layout")) return LayoutGraph(Blueprint, Graph, *Operation, OutResult, OutError);

        FGuid NodeGuid;
        UEdGraphNode* Node = nullptr;
        if (Name.StartsWith(TEXT("node.")) || Name == TEXT("pin.default"))
        {
            if (!ReadGuid(*Operation, TEXT("nodeGuid"), NodeGuid, OutError, TEXT("FBlueprintGraphOperations::Apply"))) return false;
            Node = FindNode(Graph, NodeGuid, OutError);
            if (!Node) return false;
        }
        if (Name == TEXT("node.delete"))
        {
            if (!Node->CanUserDeleteNode())
            {
                Fail(OutError, TEXT("GraphNodeDeletionForbidden"), TEXT("UE marks this node as non-deletable."), TEXT("UEdGraphNode::CanUserDeleteNode"), Blueprint, Graph);
                return false;
            }
            Graph->Modify();
            Node->Modify();
            Node->DestroyNode();
            FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
            FillResult(Blueprint, Graph, OutResult);
            return true;
        }
        if (Name == TEXT("node.move"))
        {
            double X = 0.0, Y = 0.0;
            if (!Operation->TryGetNumberField(TEXT("x"), X) || !Operation->TryGetNumberField(TEXT("y"), Y))
            {
                Fail(OutError, TEXT("GraphNodeLocationRequired"), TEXT("node.move requires numeric x and y."), TEXT("node.move"), Blueprint, Graph);
                return false;
            }
            Node->Modify();
            Node->NodePosX = FMath::RoundToInt(X);
            Node->NodePosY = FMath::RoundToInt(Y);
            Graph->NotifyGraphChanged();
            FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
            FillResult(Blueprint, Graph, OutResult);
            OutResult.NodeGuids.Add(NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
            return true;
        }
        if (Name == TEXT("node.reconstruct") || Name == TEXT("node.refresh"))
        {
            if (!EnsureK2Graph(Blueprint, Graph, OutError)) return false;
            Node->Modify();
            GetDefault<UEdGraphSchema_K2>()->ReconstructNode(*Node, false);
            Graph->NotifyGraphChanged();
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
            FillResult(Blueprint, Graph, OutResult);
            OutResult.NodeGuids.Add(NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
            return true;
        }
        if (Name == TEXT("pin.default"))
        {
            FGuid PinGuid;
            if (!ReadGuid(*Operation, TEXT("pinGuid"), PinGuid, OutError, TEXT("pin.default"))) return false;
            UEdGraphPin* Pin = Node->FindPinById(PinGuid);
            if (!Pin)
            {
                Fail(OutError, TEXT("GraphPinNotFound"), TEXT("The exact pin GUID was not found on the node."), TEXT("pin.default"), Blueprint, Graph);
                return false;
            }
            if (Pin->LinkedTo.Num())
            {
                Fail(OutError, TEXT("GraphPinDefaultLinked"), TEXT("A linked pin cannot receive a default value."), TEXT("pin.default"), Blueprint, Graph);
                return false;
            }
            FString ValueKind = TEXT("string");
            Operation->TryGetStringField(TEXT("valueKind"), ValueKind);
            FString Value;
            if (!Operation->TryGetStringField(TEXT("value"), Value))
            {
                Fail(OutError, TEXT("GraphPinDefaultInvalid"), TEXT("pin.default requires string value."), TEXT("pin.default"), Blueprint, Graph);
                return false;
            }
            const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
            Node->Modify();
            if (ValueKind == TEXT("object"))
            {
                UObject* Object = StaticLoadObject(UObject::StaticClass(), nullptr, *Value, nullptr, LOAD_NoWarn);
                if (!Object)
                {
                    Fail(OutError, TEXT("GraphPinDefaultObjectNotFound"), TEXT("The exact default object path was not found."), TEXT("StaticLoadObject"), Blueprint, Graph);
                    return false;
                }
                const FString Validation = Schema->IsPinDefaultValid(Pin, FString(), Object, FText::GetEmpty());
                if (!Validation.IsEmpty())
                {
                    Fail(OutError, TEXT("GraphPinDefaultRejected"), Validation, TEXT("UEdGraphSchema_K2::IsPinDefaultValid"), Blueprint, Graph);
                    return false;
                }
                Schema->TrySetDefaultObject(*Pin, Object);
            }
            else if (ValueKind == TEXT("text"))
            {
                const FText Text = FText::FromString(Value);
                const FString Validation = Schema->IsPinDefaultValid(Pin, FString(), nullptr, Text);
                if (!Validation.IsEmpty())
                {
                    Fail(OutError, TEXT("GraphPinDefaultRejected"), Validation, TEXT("UEdGraphSchema_K2::IsPinDefaultValid"), Blueprint, Graph);
                    return false;
                }
                Schema->TrySetDefaultText(*Pin, Text);
            }
            else if (ValueKind == TEXT("string"))
            {
                const FString Validation = Schema->IsPinDefaultValid(Pin, Value, nullptr, FText::GetEmpty());
                if (!Validation.IsEmpty())
                {
                    Fail(OutError, TEXT("GraphPinDefaultRejected"), Validation, TEXT("UEdGraphSchema_K2::IsPinDefaultValid"), Blueprint, Graph);
                    return false;
                }
                Schema->TrySetDefaultValue(*Pin, Value);
            }
            else
            {
                Fail(OutError, TEXT("GraphPinDefaultKindUnknown"), TEXT("valueKind must be string, object, or text."), TEXT("pin.default"), Blueprint, Graph);
                return false;
            }
            FillResult(Blueprint, Graph, OutResult);
            OutResult.PinGuids.Add(PinGuid.ToString(EGuidFormats::DigitsWithHyphens));
            return true;
        }
        if (Name == TEXT("link.connect") || Name == TEXT("link.disconnect"))
        {
            if (!EnsureK2Graph(Blueprint, Graph, OutError)) return false;
            UEdGraphPin* A = FindPin(Graph, *Operation, TEXT("a"), OutError);
            UEdGraphPin* B = FindPin(Graph, *Operation, TEXT("b"), OutError);
            if (!A || !B) return false;
            const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
            if (Name == TEXT("link.connect"))
            {
                if (!Schema->TryCreateConnection(A, B))
                {
                    const FPinConnectionResponse Response = Schema->CanCreateConnection(A, B);
                    Fail(OutError, TEXT("GraphConnectionRejected"), Response.Message.ToString(), TEXT("UEdGraphSchema_K2::TryCreateConnection"), Blueprint, Graph);
                    return false;
                }
            }
            else
            {
                if (!A->LinkedTo.Contains(B))
                {
                    Fail(OutError, TEXT("GraphConnectionNotFound"), TEXT("The exact pin connection does not exist."), TEXT("link.disconnect"), Blueprint, Graph);
                    return false;
                }
                Schema->BreakSinglePinLink(A, B);
            }
            FillResult(Blueprint, Graph, OutResult);
            OutResult.PinGuids.Add(A->PinId.ToString(EGuidFormats::DigitsWithHyphens));
            OutResult.PinGuids.Add(B->PinId.ToString(EGuidFormats::DigitsWithHyphens));
            return true;
        }

        Fail(OutError, TEXT("GraphOperationUnknown"), FString::Printf(TEXT("Unknown graph operation '%s'."), *Name), TEXT("FBlueprintGraphOperations::Apply"), Blueprint, Graph);
        return false;
    }
}
