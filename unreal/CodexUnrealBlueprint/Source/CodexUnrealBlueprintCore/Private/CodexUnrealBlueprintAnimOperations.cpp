#include "CodexUnrealBlueprintAnimOperations.h"

#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimStateConduitNode.h"
#include "AnimStateEntryNode.h"
#include "AnimStateNode.h"
#include "AnimStateNodeBase.h"
#include "AnimStateTransitionNode.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Animation/Skeleton.h"
#include "AnimationGraphSchema.h"
#include "AnimationStateMachineGraph.h"
#include "AnimationStateMachineSchema.h"
#include "AnimationTransitionSchema.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/SecureHash.h"
#include "CodexUnrealBlueprintActionCatalog.h"
#include "CodexUnrealBlueprintTypeSystem.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UObjectGlobals.h"

namespace CodexUnrealBlueprint
{
    namespace
    {
        FString GuidString(const FGuid& Guid)
        {
            return Guid.ToString(EGuidFormats::DigitsWithHyphens).ToLower();
        }

        void Fail(FAnimOperationError& Error, const TCHAR* Code, const FString& Message, const TCHAR* Callsite,
            UAnimBlueprint* Blueprint = nullptr, UEdGraph* Graph = nullptr, UEdGraphNode* Node = nullptr,
            UEdGraphPin* Pin = nullptr, const int32 OperationIndex = INDEX_NONE)
        {
            Error.Code = Code;
            Error.Message = Message;
            Error.AssetPath = Blueprint ? Blueprint->GetPathName() : FString();
            Error.GraphPath = Graph ? Graph->GetPathName() : FString();
            Error.NodeGuid = Node ? GuidString(Node->NodeGuid) : FString();
            Error.PinGuid = Pin ? GuidString(Pin->PinId) : FString();
            Error.UECallsite = Callsite;
            Error.OperationIndex = OperationIndex;
        }

        void FromActionError(const FGraphActionError& Source, FAnimOperationError& Error, const int32 OperationIndex)
        {
            Error.Code = Source.Code.IsEmpty() ? TEXT("ANIM_ACTION_FAILED") : Source.Code;
            Error.Message = Source.Message;
            Error.AssetPath = Source.AssetPath;
            Error.GraphPath = Source.GraphPath;
            Error.UECallsite = Source.UECallsite;
            Error.OperationIndex = OperationIndex;
            Error.Candidates = Source.Candidates;
        }

        void FromOperationError(const FBlueprintOperationError& Source, FAnimOperationError& Error)
        {
            Error.Code = Source.Code;
            Error.Message = Source.Message;
            Error.AssetPath = Source.AssetPath;
            Error.UECallsite = Source.UECallsite;
            Error.OperationIndex = Source.OperationIndex;
            Error.Candidates = Source.Details;
        }

        bool RequiredString(const FJsonObject& Json, const TCHAR* Field, FString& Out,
            FAnimOperationError& Error, UAnimBlueprint* Blueprint, const int32 OperationIndex)
        {
            if (!Json.TryGetStringField(Field, Out) || Out.TrimStartAndEnd().IsEmpty())
            {
                Fail(Error, TEXT("ANIM_INVALID_ARGUMENT"),
                    FString::Printf(TEXT("'%s' must be a non-empty string."), Field),
                    TEXT("RequiredString"), Blueprint, nullptr, nullptr, nullptr, OperationIndex);
                return false;
            }
            return true;
        }

        bool OptionalGuid(const FJsonObject& Json, const TCHAR* Field, FGuid& Out, bool& bPresent,
            FAnimOperationError& Error, UAnimBlueprint* Blueprint, const int32 OperationIndex)
        {
            FString Value;
            bPresent = Json.TryGetStringField(Field, Value);
            if (!bPresent) return true;
            if (!FGuid::Parse(Value, Out) || !Out.IsValid())
            {
                Fail(Error, TEXT("ANIM_INVALID_GUID"),
                    FString::Printf(TEXT("'%s' is not a valid GUID."), Field),
                    TEXT("FGuid::Parse"), Blueprint, nullptr, nullptr, nullptr, OperationIndex);
                return false;
            }
            return true;
        }

        bool RequiredGuid(const FJsonObject& Json, const TCHAR* Field, FGuid& Out,
            FAnimOperationError& Error, UAnimBlueprint* Blueprint, const int32 OperationIndex)
        {
            bool bPresent = false;
            if (!OptionalGuid(Json, Field, Out, bPresent, Error, Blueprint, OperationIndex)) return false;
            if (!bPresent)
            {
                Fail(Error, TEXT("ANIM_INVALID_GUID"), FString::Printf(TEXT("'%s' is required."), Field),
                    TEXT("RequiredGuid"), Blueprint, nullptr, nullptr, nullptr, OperationIndex);
                return false;
            }
            return true;
        }

        UEdGraph* FindGraph(UAnimBlueprint* Blueprint, const FJsonObject& Json,
            FAnimOperationError& Error, const int32 OperationIndex)
        {
            FString GuidText;
            FString Name;
            const bool bGuid = Json.TryGetStringField(TEXT("graphGuid"), GuidText);
            const bool bName = Json.TryGetStringField(TEXT("graphName"), Name);
            if (bGuid == bName)
            {
                Fail(Error, TEXT("ANIM_GRAPH_REFERENCE_INVALID"),
                    TEXT("Specify exactly one of graphGuid or graphName."), TEXT("FindGraph"), Blueprint,
                    nullptr, nullptr, nullptr, OperationIndex);
                return nullptr;
            }
            FGuid Guid;
            if (bGuid && (!FGuid::Parse(GuidText, Guid) || !Guid.IsValid()))
            {
                Fail(Error, TEXT("ANIM_INVALID_GUID"), TEXT("graphGuid is not a valid GUID."),
                    TEXT("FGuid::Parse"), Blueprint, nullptr, nullptr, nullptr, OperationIndex);
                return nullptr;
            }
            TArray<UEdGraph*> Graphs;
            Blueprint->GetAllGraphs(Graphs);
            TArray<UEdGraph*> Matches;
            for (UEdGraph* Graph : Graphs)
            {
                if (Graph && ((bGuid && Graph->GraphGuid == Guid) || (bName && Graph->GetName() == Name)))
                    Matches.Add(Graph);
            }
            if (Matches.Num() != 1)
            {
                Fail(Error, Matches.Num() == 0 ? TEXT("ANIM_GRAPH_NOT_FOUND") : TEXT("ANIM_GRAPH_AMBIGUOUS"),
                    Matches.Num() == 0 ? TEXT("The exact animation graph was not found.")
                                       : TEXT("graphName matches multiple graphs; use graphGuid."),
                    TEXT("UBlueprint::GetAllGraphs"), Blueprint, nullptr, nullptr, nullptr, OperationIndex);
                for (const UEdGraph* Match : Matches) Error.Candidates.Add(GuidString(Match->GraphGuid));
                return nullptr;
            }
            return Matches[0];
        }

        UEdGraphNode* FindNode(UEdGraph* Graph, const FGuid& Guid, FAnimOperationError& Error,
            UAnimBlueprint* Blueprint, const int32 OperationIndex)
        {
            for (UEdGraphNode* Node : Graph->Nodes)
                if (Node && Node->NodeGuid == Guid) return Node;
            Fail(Error, TEXT("ANIM_NODE_NOT_FOUND"), FString::Printf(TEXT("Node '%s' was not found."), *GuidString(Guid)),
                TEXT("UEdGraph::Nodes"), Blueprint, Graph, nullptr, nullptr, OperationIndex);
            return nullptr;
        }

        UEdGraphPin* FindPin(UEdGraph* Graph, const FJsonObject& Json, const TCHAR* Prefix,
            FAnimOperationError& Error, UAnimBlueprint* Blueprint, const int32 OperationIndex)
        {
            FGuid NodeGuid;
            FGuid PinGuid;
            if (!RequiredGuid(Json, *(FString(Prefix) + TEXT("NodeGuid")), NodeGuid, Error, Blueprint, OperationIndex)
                || !RequiredGuid(Json, *(FString(Prefix) + TEXT("PinGuid")), PinGuid, Error, Blueprint, OperationIndex))
                return nullptr;
            UEdGraphNode* Node = FindNode(Graph, NodeGuid, Error, Blueprint, OperationIndex);
            if (!Node) return nullptr;
            UEdGraphPin* Pin = Node->FindPinById(PinGuid);
            if (!Pin)
                Fail(Error, TEXT("ANIM_PIN_NOT_FOUND"), FString::Printf(TEXT("Pin '%s' was not found."), *GuidString(PinGuid)),
                    TEXT("UEdGraphNode::FindPinById"), Blueprint, Graph, Node, nullptr, OperationIndex);
            return Pin;
        }

        bool RequireSchema(UEdGraph* Graph, UClass* ExactSchema, FAnimOperationError& Error,
            UAnimBlueprint* Blueprint, const int32 OperationIndex, const TCHAR* Callsite)
        {
            const UEdGraphSchema* Schema = Graph ? Graph->GetSchema() : nullptr;
            if (!Schema || Schema->GetClass() != ExactSchema)
            {
                Fail(Error, TEXT("ANIM_SCHEMA_MISMATCH"),
                    FString::Printf(TEXT("Graph requires exact schema '%s'; actual schema is '%s'."),
                        *ExactSchema->GetPathName(), Schema ? *Schema->GetClass()->GetPathName() : TEXT("null")),
                    Callsite, Blueprint, Graph, nullptr, nullptr, OperationIndex);
                return false;
            }
            return true;
        }

        bool RequireEventGraph(UAnimBlueprint* Blueprint, UEdGraph* Graph, FAnimOperationError& Error,
            const int32 OperationIndex)
        {
            if (!RequireSchema(Graph, UEdGraphSchema_K2::StaticClass(), Error, Blueprint, OperationIndex,
                TEXT("RequireEventGraph"))) return false;
            if (!Blueprint->UbergraphPages.Contains(Graph))
            {
                Fail(Error, TEXT("ANIM_EVENT_GRAPH_REQUIRED"), TEXT("The graph is not an Animation Blueprint EventGraph."),
                    TEXT("UBlueprint::UbergraphPages"), Blueprint, Graph, nullptr, nullptr, OperationIndex);
                return false;
            }
            return true;
        }

        void FillResult(UAnimBlueprint* Blueprint, UEdGraph* Graph, FAnimOperationResult& Result, const bool bStructural = false)
        {
            if (bStructural) FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
            else FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
            Result.bChanged = true;
            Result.AssetPath = Blueprint->GetPathName();
            Result.ImpactPackages.AddUnique(Blueprint->GetOutermost()->GetName());
            if (Graph)
            {
                Result.GraphPath = Graph->GetPathName();
                Result.GraphGuid = GuidString(Graph->GraphGuid);
            }
        }

        bool AssignRequestedGuid(UEdGraph* Graph, UEdGraphNode* Node, const FJsonObject& Op,
            FAnimOperationError& Error, UAnimBlueprint* Blueprint, const int32 OperationIndex)
        {
            FGuid Requested;
            bool bPresent = false;
            if (!OptionalGuid(Op, TEXT("requestedNodeGuid"), Requested, bPresent, Error, Blueprint, OperationIndex)) return false;
            if (!bPresent) return true;
            for (const UEdGraphNode* Existing : Graph->Nodes)
            {
                if (Existing && Existing != Node && Existing->NodeGuid == Requested)
                {
                    Fail(Error, TEXT("ANIM_GUID_CONFLICT"), TEXT("requestedNodeGuid already exists in the graph."),
                        TEXT("AssignRequestedGuid"), Blueprint, Graph, Node, nullptr, OperationIndex);
                    return false;
                }
            }
            Node->NodeGuid = Requested;
            return true;
        }

        bool SpawnActionNode(UAnimBlueprint* Blueprint, UEdGraph* Graph, const FJsonObject& Op,
            FAnimOperationResult& Result, FAnimOperationError& Error, const int32 OperationIndex)
        {
            FString ActionId;
            if (!RequiredString(Op, TEXT("actionId"), ActionId, Error, Blueprint, OperationIndex)) return false;
            double X = 0.0;
            double Y = 0.0;
            Op.TryGetNumberField(TEXT("x"), X);
            Op.TryGetNumberField(TEXT("y"), Y);
            FGraphActionError ActionError;
            UEdGraphNode* Node = nullptr;
            if (!FBlueprintGraphActionCatalog::Spawn(ActionId, Blueprint, Graph, FVector2D(X, Y), Node, ActionError))
            {
                FromActionError(ActionError, Error, OperationIndex);
                return false;
            }
            if (!Node || Node->GetGraph() != Graph)
            {
                Fail(Error, TEXT("ANIM_SPAWNER_INVALID_RESULT"), TEXT("The resolved spawner did not create a node in the requested graph."),
                    TEXT("FBlueprintGraphActionCatalog::Spawn"), Blueprint, Graph, Node, nullptr, OperationIndex);
                return false;
            }
            if (!AssignRequestedGuid(Graph, Node, Op, Error, Blueprint, OperationIndex))
            {
                FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
                return false;
            }
            FillResult(Blueprint, Graph, Result, true);
            Result.NodeGuids.Add(GuidString(Node->NodeGuid));
            for (const UEdGraphPin* Pin : Node->Pins) if (Pin) Result.PinGuids.Add(GuidString(Pin->PinId));
            return true;
        }

        bool SpawnStateLike(UAnimBlueprint* Blueprint, UEdGraph* Graph, const FJsonObject& Op,
            const bool bConduit, FAnimOperationResult& Result, FAnimOperationError& Error, const int32 OperationIndex)
        {
            if (!RequireSchema(Graph, UAnimationStateMachineSchema::StaticClass(), Error, Blueprint, OperationIndex,
                TEXT("FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate"))) return false;
            double X = 0.0;
            double Y = 0.0;
            Op.TryGetNumberField(TEXT("x"), X);
            Op.TryGetNumberField(TEXT("y"), Y);
            UAnimStateNodeBase* Node = bConduit
                ? static_cast<UAnimStateNodeBase*>(FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateConduitNode>(
                    Graph, NewObject<UAnimStateConduitNode>(), FVector2D(X, Y), false))
                : static_cast<UAnimStateNodeBase*>(FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateNode>(
                    Graph, NewObject<UAnimStateNode>(), FVector2D(X, Y), false));
            if (!Node || !Node->GetBoundGraph())
            {
                Fail(Error, TEXT("ANIM_STATE_SPAWN_FAILED"), TEXT("Animation state schema failed to create the node and bound graph."),
                    TEXT("FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate"), Blueprint, Graph, Node, nullptr, OperationIndex);
                return false;
            }
            FString Name;
            if (Op.TryGetStringField(TEXT("name"), Name) && !Name.IsEmpty())
            {
                FBlueprintEditorUtils::RenameGraph(Node->GetBoundGraph(), Name);
                if (Node->GetBoundGraph()->GetName() != Name)
                {
                    FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
                    Fail(Error, TEXT("ANIM_GRAPH_NAME_CONFLICT"), TEXT("UE could not assign the requested state or conduit name exactly."),
                        TEXT("FBlueprintEditorUtils::RenameGraph"), Blueprint, Graph, Node, nullptr, OperationIndex);
                    return false;
                }
            }
            if (!AssignRequestedGuid(Graph, Node, Op, Error, Blueprint, OperationIndex))
            {
                FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
                return false;
            }
            FillResult(Blueprint, Graph, Result, true);
            Result.NodeGuids.Add(GuidString(Node->NodeGuid));
            Result.Data = MakeShared<FJsonObject>();
            Result.Data->SetStringField(TEXT("boundGraphGuid"), GuidString(Node->GetBoundGraph()->GraphGuid));
            Result.Data->SetStringField(TEXT("boundGraphPath"), Node->GetBoundGraph()->GetPathName());
            return true;
        }

        bool RenameStateLike(UAnimBlueprint* Blueprint, UEdGraph* Graph, const FJsonObject& Op,
            FAnimOperationResult& Result, FAnimOperationError& Error, const int32 OperationIndex)
        {
            FGuid Guid;
            FString Name;
            if (!RequiredGuid(Op, TEXT("nodeGuid"), Guid, Error, Blueprint, OperationIndex)
                || !RequiredString(Op, TEXT("name"), Name, Error, Blueprint, OperationIndex)) return false;
            UAnimStateNodeBase* Node = Cast<UAnimStateNodeBase>(FindNode(Graph, Guid, Error, Blueprint, OperationIndex));
            if (!Node || (!Cast<UAnimStateNode>(Node) && !Cast<UAnimStateConduitNode>(Node)))
            {
                if (!Error.IsSet()) Fail(Error, TEXT("ANIM_STATE_NODE_REQUIRED"), TEXT("nodeGuid must identify a state or conduit."),
                    TEXT("Cast<UAnimStateNodeBase>"), Blueprint, Graph, Node, nullptr, OperationIndex);
                return false;
            }
            if (!Node->GetBoundGraph())
            {
                Fail(Error, TEXT("ANIM_BOUND_GRAPH_MISSING"), TEXT("The state node has no bound graph."),
                    TEXT("UAnimStateNodeBase::GetBoundGraph"), Blueprint, Graph, Node, nullptr, OperationIndex);
                return false;
            }
            FBlueprintEditorUtils::RenameGraph(Node->GetBoundGraph(), Name);
            if (Node->GetBoundGraph()->GetName() != Name)
            {
                Fail(Error, TEXT("ANIM_GRAPH_NAME_CONFLICT"), TEXT("UE could not assign the requested state or conduit name exactly."),
                    TEXT("FBlueprintEditorUtils::RenameGraph"), Blueprint, Graph, Node, nullptr, OperationIndex);
                return false;
            }
            FillResult(Blueprint, Graph, Result, true);
            Result.NodeGuids.Add(GuidString(Node->NodeGuid));
            return true;
        }

        bool AddTransition(UAnimBlueprint* Blueprint, UEdGraph* Graph, const FJsonObject& Op,
            FAnimOperationResult& Result, FAnimOperationError& Error, const int32 OperationIndex)
        {
            if (!RequireSchema(Graph, UAnimationStateMachineSchema::StaticClass(), Error, Blueprint, OperationIndex,
                TEXT("FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate"))) return false;
            FGuid FromGuid;
            FGuid ToGuid;
            if (!RequiredGuid(Op, TEXT("fromNodeGuid"), FromGuid, Error, Blueprint, OperationIndex)
                || !RequiredGuid(Op, TEXT("toNodeGuid"), ToGuid, Error, Blueprint, OperationIndex)) return false;
            UAnimStateNodeBase* From = Cast<UAnimStateNodeBase>(FindNode(Graph, FromGuid, Error, Blueprint, OperationIndex));
            if (!From) return false;
            UAnimStateNodeBase* To = Cast<UAnimStateNodeBase>(FindNode(Graph, ToGuid, Error, Blueprint, OperationIndex));
            if (!To) return false;
            if (Cast<UAnimStateTransitionNode>(From) || Cast<UAnimStateTransitionNode>(To) || From == To
                || !From->GetOutputPin() || !To->GetInputPin())
            {
                Fail(Error, TEXT("ANIM_TRANSITION_ENDPOINT_INVALID"),
                    TEXT("Transition endpoints must be distinct state/conduit nodes with schema pins."),
                    TEXT("UAnimStateNodeBase::GetInputPin"), Blueprint, Graph, From, nullptr, OperationIndex);
                return false;
            }
            const FPinConnectionResponse Response = Graph->GetSchema()->CanCreateConnection(From->GetOutputPin(), To->GetInputPin());
            if (Response.Response == CONNECT_RESPONSE_DISALLOW)
            {
                Fail(Error, TEXT("ANIM_TRANSITION_REJECTED"), Response.Message.ToString(),
                    TEXT("UAnimationStateMachineSchema::CanCreateConnection"), Blueprint, Graph, From,
                    From->GetOutputPin(), OperationIndex);
                return false;
            }
            UAnimStateTransitionNode* Transition = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateTransitionNode>(
                Graph, NewObject<UAnimStateTransitionNode>(), FVector2D((From->NodePosX + To->NodePosX) * 0.5f,
                    (From->NodePosY + To->NodePosY) * 0.5f), false);
            if (!Transition || !Transition->GetBoundGraph())
            {
                Fail(Error, TEXT("ANIM_TRANSITION_SPAWN_FAILED"), TEXT("Animation state schema failed to create the transition."),
                    TEXT("FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate"), Blueprint, Graph, Transition, nullptr, OperationIndex);
                return false;
            }
            Transition->CreateConnections(From, To);
            if (!AssignRequestedGuid(Graph, Transition, Op, Error, Blueprint, OperationIndex))
            {
                FBlueprintEditorUtils::RemoveNode(Blueprint, Transition, true);
                return false;
            }
            FillResult(Blueprint, Graph, Result, true);
            Result.NodeGuids.Add(GuidString(Transition->NodeGuid));
            Result.Data = MakeShared<FJsonObject>();
            Result.Data->SetStringField(TEXT("ruleGraphGuid"), GuidString(Transition->GetBoundGraph()->GraphGuid));
            Result.Data->SetStringField(TEXT("ruleGraphPath"), Transition->GetBoundGraph()->GetPathName());
            return true;
        }

        bool RemoveNode(UAnimBlueprint* Blueprint, UEdGraph* Graph, const FJsonObject& Op,
            FAnimOperationResult& Result, FAnimOperationError& Error, const int32 OperationIndex,
            UClass* RequiredClass = nullptr)
        {
            FGuid Guid;
            if (!RequiredGuid(Op, TEXT("nodeGuid"), Guid, Error, Blueprint, OperationIndex)) return false;
            UEdGraphNode* Node = FindNode(Graph, Guid, Error, Blueprint, OperationIndex);
            if (!Node) return false;
            if (RequiredClass && !Node->IsA(RequiredClass))
            {
                Fail(Error, TEXT("ANIM_NODE_KIND_MISMATCH"),
                    FString::Printf(TEXT("Node must be a '%s'."), *RequiredClass->GetPathName()),
                    TEXT("UObject::IsA"), Blueprint, Graph, Node, nullptr, OperationIndex);
                return false;
            }
            if (!Node->CanUserDeleteNode())
            {
                Fail(Error, TEXT("ANIM_NODE_DELETE_FORBIDDEN"), TEXT("UE marks this node as non-deletable."),
                    TEXT("UEdGraphNode::CanUserDeleteNode"), Blueprint, Graph, Node, nullptr, OperationIndex);
                return false;
            }
            FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
            FillResult(Blueprint, Graph, Result, true);
            Result.NodeGuids.Add(GuidString(Guid));
            return true;
        }

        bool SetNodeProperty(UAnimBlueprint* Blueprint, UEdGraph* Graph, const FJsonObject& Op,
            FAnimOperationResult& Result, FAnimOperationError& Error, const int32 OperationIndex,
            UClass* RequiredClass = nullptr)
        {
            FGuid Guid;
            FString Path;
            if (!RequiredGuid(Op, TEXT("nodeGuid"), Guid, Error, Blueprint, OperationIndex)
                || !RequiredString(Op, TEXT("propertyPath"), Path, Error, Blueprint, OperationIndex)) return false;
            UEdGraphNode* Node = FindNode(Graph, Guid, Error, Blueprint, OperationIndex);
            if (!Node) return false;
            if (RequiredClass && !Node->IsA(RequiredClass))
            {
                Fail(Error, TEXT("ANIM_NODE_KIND_MISMATCH"),
                    FString::Printf(TEXT("Node must be a '%s'."), *RequiredClass->GetPathName()),
                    TEXT("UObject::IsA"), Blueprint, Graph, Node, nullptr, OperationIndex);
                return false;
            }
            const TSharedPtr<FJsonValue>* Value = Op.Values.Find(TEXT("value"));
            if (!Value || !Value->IsValid())
            {
                Fail(Error, TEXT("ANIM_INVALID_ARGUMENT"), TEXT("'value' is required."), TEXT("FJsonObject::Values"),
                    Blueprint, Graph, Node, nullptr, OperationIndex);
                return false;
            }
            FBlueprintOperationError TypeError;
            if (!FBlueprintTypeSystem::SetPropertyValue(Node, Path, *Value, TypeError, Blueprint->GetPathName(), OperationIndex))
            {
                FromOperationError(TypeError, Error);
                Error.GraphPath = Graph->GetPathName();
                Error.NodeGuid = GuidString(Node->NodeGuid);
                return false;
            }
            Node->ReconstructNode();
            FillResult(Blueprint, Graph, Result, true);
            Result.NodeGuids.Add(GuidString(Node->NodeGuid));
            for (const UEdGraphPin* Pin : Node->Pins) if (Pin) Result.PinGuids.Add(GuidString(Pin->PinId));
            return true;
        }

        bool MoveNode(UAnimBlueprint* Blueprint, UEdGraph* Graph, const FJsonObject& Op,
            FAnimOperationResult& Result, FAnimOperationError& Error, const int32 OperationIndex)
        {
            FGuid Guid;
            double X = 0.0;
            double Y = 0.0;
            if (!RequiredGuid(Op, TEXT("nodeGuid"), Guid, Error, Blueprint, OperationIndex)
                || !Op.TryGetNumberField(TEXT("x"), X) || !Op.TryGetNumberField(TEXT("y"), Y))
            {
                if (!Error.IsSet()) Fail(Error, TEXT("ANIM_INVALID_ARGUMENT"), TEXT("x and y must be numbers."),
                    TEXT("FJsonObject::TryGetNumberField"), Blueprint, Graph, nullptr, nullptr, OperationIndex);
                return false;
            }
            UEdGraphNode* Node = FindNode(Graph, Guid, Error, Blueprint, OperationIndex);
            if (!Node) return false;
            Node->NodePosX = FMath::RoundToInt(X);
            Node->NodePosY = FMath::RoundToInt(Y);
            FillResult(Blueprint, Graph, Result);
            Result.NodeGuids.Add(GuidString(Node->NodeGuid));
            return true;
        }

        bool ConnectPose(UAnimBlueprint* Blueprint, UEdGraph* Graph, const FJsonObject& Op, const bool bConnect,
            FAnimOperationResult& Result, FAnimOperationError& Error, const int32 OperationIndex)
        {
            if (!RequireSchema(Graph, UAnimationGraphSchema::StaticClass(), Error, Blueprint, OperationIndex,
                TEXT("UAnimationGraphSchema::TryCreateConnection"))) return false;
            UEdGraphPin* A = FindPin(Graph, Op, TEXT("from"), Error, Blueprint, OperationIndex);
            UEdGraphPin* B = FindPin(Graph, Op, TEXT("to"), Error, Blueprint, OperationIndex);
            if (!A || !B) return false;
            if (!UAnimationGraphSchema::IsPosePin(A->PinType) || !UAnimationGraphSchema::IsPosePin(B->PinType))
            {
                Fail(Error, TEXT("ANIM_POSE_PIN_REQUIRED"), TEXT("Both endpoints must be animation pose pins."),
                    TEXT("UAnimationGraphSchema::IsPosePin"), Blueprint, Graph, A->GetOwningNode(), A, OperationIndex);
                return false;
            }
            const UAnimationGraphSchema* Schema = CastChecked<UAnimationGraphSchema>(Graph->GetSchema());
            if (bConnect)
            {
                const FPinConnectionResponse Response = Schema->CanCreateConnection(A, B);
                if (Response.Response == CONNECT_RESPONSE_DISALLOW || !Schema->TryCreateConnection(A, B))
                {
                    Fail(Error, TEXT("ANIM_POSE_LINK_REJECTED"), Response.Message.ToString(),
                        TEXT("UAnimationGraphSchema::TryCreateConnection"), Blueprint, Graph, A->GetOwningNode(), A, OperationIndex);
                    return false;
                }
            }
            else
            {
                if (!A->LinkedTo.Contains(B))
                {
                    Fail(Error, TEXT("ANIM_POSE_LINK_NOT_FOUND"), TEXT("The exact pose link does not exist."),
                        TEXT("UEdGraphPin::LinkedTo"), Blueprint, Graph, A->GetOwningNode(), A, OperationIndex);
                    return false;
                }
                Schema->BreakSinglePinLink(A, B);
            }
            FillResult(Blueprint, Graph, Result);
            Result.PinGuids.Add(GuidString(A->PinId));
            Result.PinGuids.Add(GuidString(B->PinId));
            return true;
        }

        bool ConnectSchemaPins(UAnimBlueprint* Blueprint, UEdGraph* Graph, const FJsonObject& Op, const bool bConnect,
            FAnimOperationResult& Result, FAnimOperationError& Error, const int32 OperationIndex)
        {
            UEdGraphPin* A = FindPin(Graph, Op, TEXT("from"), Error, Blueprint, OperationIndex);
            UEdGraphPin* B = FindPin(Graph, Op, TEXT("to"), Error, Blueprint, OperationIndex);
            if (!A || !B) return false;
            const UEdGraphSchema* Schema = Graph->GetSchema();
            if (bConnect)
            {
                const FPinConnectionResponse Response = Schema->CanCreateConnection(A, B);
                if (Response.Response == CONNECT_RESPONSE_DISALLOW || !Schema->TryCreateConnection(A, B))
                {
                    Fail(Error, TEXT("ANIM_SCHEMA_LINK_REJECTED"), Response.Message.ToString(),
                        TEXT("UEdGraphSchema::TryCreateConnection"), Blueprint, Graph, A->GetOwningNode(), A, OperationIndex);
                    return false;
                }
            }
            else
            {
                if (!A->LinkedTo.Contains(B))
                {
                    Fail(Error, TEXT("ANIM_SCHEMA_LINK_NOT_FOUND"), TEXT("The exact schema link does not exist."),
                        TEXT("UEdGraphPin::LinkedTo"), Blueprint, Graph, A->GetOwningNode(), A, OperationIndex);
                    return false;
                }
                Schema->BreakSinglePinLink(A, B);
            }
            FillResult(Blueprint, Graph, Result);
            Result.PinGuids.Add(GuidString(A->PinId));
            Result.PinGuids.Add(GuidString(B->PinId));
            return true;
        }

        bool SetStateMachineEntry(UAnimBlueprint* Blueprint, UEdGraph* Graph, const FJsonObject& Op,
            FAnimOperationResult& Result, FAnimOperationError& Error, const int32 OperationIndex)
        {
            if (!RequireSchema(Graph, UAnimationStateMachineSchema::StaticClass(), Error, Blueprint, OperationIndex,
                TEXT("UAnimationStateMachineSchema::TryCreateConnection"))) return false;
            UAnimationStateMachineGraph* MachineGraph = Cast<UAnimationStateMachineGraph>(Graph);
            if (!MachineGraph || !MachineGraph->EntryNode || MachineGraph->EntryNode->Pins.Num() == 0
                || !MachineGraph->EntryNode->Pins[0])
            {
                Fail(Error, TEXT("ANIM_STATE_MACHINE_ENTRY_MISSING"), TEXT("State machine graph has no valid entry node."),
                    TEXT("UAnimationStateMachineGraph::EntryNode"), Blueprint, Graph, nullptr, nullptr, OperationIndex);
                return false;
            }
            FGuid StateGuid;
            if (!RequiredGuid(Op, TEXT("stateNodeGuid"), StateGuid, Error, Blueprint, OperationIndex)) return false;
            UAnimStateNodeBase* State = Cast<UAnimStateNodeBase>(FindNode(Graph, StateGuid, Error, Blueprint, OperationIndex));
            if (!State || Cast<UAnimStateTransitionNode>(State) || !State->GetInputPin())
            {
                if (!Error.IsSet()) Fail(Error, TEXT("ANIM_ENTRY_TARGET_INVALID"),
                    TEXT("stateNodeGuid must identify a state or conduit with an input pin."),
                    TEXT("UAnimStateNodeBase::GetInputPin"), Blueprint, Graph, State, nullptr, OperationIndex);
                return false;
            }
            UEdGraphPin* EntryPin = MachineGraph->EntryNode->Pins[0];
            const UAnimationStateMachineSchema* Schema = CastChecked<UAnimationStateMachineSchema>(Graph->GetSchema());
            const FPinConnectionResponse Response = Schema->CanCreateConnection(EntryPin, State->GetInputPin());
            if (Response.Response == CONNECT_RESPONSE_DISALLOW || !Schema->TryCreateConnection(EntryPin, State->GetInputPin()))
            {
                Fail(Error, TEXT("ANIM_ENTRY_LINK_REJECTED"), Response.Message.ToString(),
                    TEXT("UAnimationStateMachineSchema::TryCreateConnection"), Blueprint, Graph,
                    MachineGraph->EntryNode, EntryPin, OperationIndex);
                return false;
            }
            FillResult(Blueprint, Graph, Result, true);
            Result.NodeGuids.Add(GuidString(State->NodeGuid));
            Result.PinGuids.Add(GuidString(EntryPin->PinId));
            Result.PinGuids.Add(GuidString(State->GetInputPin()->PinId));
            return true;
        }

        bool ParseVariable(const FJsonObject& Op, FBlueprintVariableDefinition& Definition,
            FAnimOperationError& Error, UAnimBlueprint* Blueprint, const int32 OperationIndex)
        {
            FString Name;
            if (!RequiredString(Op, TEXT("name"), Name, Error, Blueprint, OperationIndex)) return false;
            Definition.Name = FName(*Name);
            const TSharedPtr<FJsonObject>* TypeJson = nullptr;
            if (!Op.TryGetObjectField(TEXT("type"), TypeJson) || !TypeJson || !TypeJson->IsValid())
            {
                Fail(Error, TEXT("ANIM_VARIABLE_TYPE_REQUIRED"), TEXT("Variable operations require a 'type' object."),
                    TEXT("FJsonObject::TryGetObjectField"), Blueprint, nullptr, nullptr, nullptr, OperationIndex);
                return false;
            }
            FBlueprintOperationError TypeError;
            if (!FBlueprintTypeSystem::ParsePinType(*TypeJson, Definition.Type, TypeError,
                Blueprint->GetPathName(), OperationIndex))
            {
                FromOperationError(TypeError, Error);
                return false;
            }
            Op.TryGetStringField(TEXT("category"), Definition.Category);
            Op.TryGetStringField(TEXT("tooltip"), Definition.Tooltip);
            Op.TryGetBoolField(TEXT("instanceEditable"), Definition.bInstanceEditable);
            Op.TryGetBoolField(TEXT("private"), Definition.bPrivate);
            Op.TryGetBoolField(TEXT("transient"), Definition.bTransient);
            const TSharedPtr<FJsonValue>* Default = Op.Values.Find(TEXT("default"));
            if (Default) Definition.DefaultValue = *Default;
            return true;
        }

        bool ApplyVariable(UAnimBlueprint* Blueprint, const FJsonObject& Op, const FString& Name,
            FAnimOperationResult& Result, FAnimOperationError& Error, const int32 OperationIndex)
        {
            FBlueprintOperationResult VariableResult;
            if (Name == TEXT("anim.variable.remove"))
            {
                FString VariableName;
                if (!RequiredString(Op, TEXT("name"), VariableName, Error, Blueprint, OperationIndex)) return false;
                VariableResult = FBlueprintTypeSystem::RemoveVariable(Blueprint, FName(*VariableName), OperationIndex);
            }
            else
            {
                FBlueprintVariableDefinition Definition;
                if (!ParseVariable(Op, Definition, Error, Blueprint, OperationIndex)) return false;
                if (Name == TEXT("anim.variable.add"))
                    VariableResult = FBlueprintTypeSystem::AddVariable(Blueprint, Definition, OperationIndex);
                else
                {
                    FString ExistingName;
                    if (!RequiredString(Op, TEXT("existingName"), ExistingName, Error, Blueprint, OperationIndex)) return false;
                    VariableResult = FBlueprintTypeSystem::UpdateVariable(Blueprint, FName(*ExistingName), Definition, OperationIndex);
                }
            }
            if (!VariableResult.bSuccess)
            {
                if (VariableResult.Error.IsSet()) FromOperationError(VariableResult.Error.GetValue(), Error);
                else Fail(Error, TEXT("ANIM_VARIABLE_OPERATION_FAILED"), TEXT("Variable operation failed without an error payload."),
                    TEXT("FBlueprintTypeSystem"), Blueprint, nullptr, nullptr, nullptr, OperationIndex);
                return false;
            }
            FillResult(Blueprint, nullptr, Result, true);
            Result.Data = VariableResult.Data;
            return true;
        }

        FString SchemaKind(const UEdGraph* Graph)
        {
            if (!Graph || !Graph->GetSchema()) return TEXT("unknown");
            const UClass* Class = Graph->GetSchema()->GetClass();
            if (Class == UAnimationGraphSchema::StaticClass()) return TEXT("animGraph");
            if (Class == UAnimationStateMachineSchema::StaticClass()) return TEXT("stateMachine");
            if (Class == UAnimationTransitionSchema::StaticClass()) return TEXT("transitionRule");
            if (Class == UEdGraphSchema_K2::StaticClass()) return TEXT("eventGraph");
            return Class->GetPathName();
        }

        TSharedRef<FJsonObject> PinSnapshot(const UEdGraphPin* Pin)
        {
            TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
            Json->SetStringField(TEXT("guid"), GuidString(Pin->PinId));
            Json->SetStringField(TEXT("name"), Pin->PinName.ToString());
            Json->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
            Json->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
            Json->SetStringField(TEXT("subCategory"), Pin->PinType.PinSubCategory.ToString());
            if (Pin->PinType.PinSubCategoryObject.IsValid())
                Json->SetStringField(TEXT("subCategoryObject"), Pin->PinType.PinSubCategoryObject->GetPathName());
            TArray<FString> LinkIds;
            for (const UEdGraphPin* Linked : Pin->LinkedTo) if (Linked) LinkIds.Add(GuidString(Linked->PinId));
            LinkIds.Sort();
            TArray<TSharedPtr<FJsonValue>> Links;
            for (const FString& Link : LinkIds) Links.Add(MakeShared<FJsonValueString>(Link));
            Json->SetArrayField(TEXT("links"), Links);
            return Json;
        }

        TSharedRef<FJsonObject> NodeSnapshot(const UEdGraphNode* Node)
        {
            TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
            Json->SetStringField(TEXT("guid"), GuidString(Node->NodeGuid));
            Json->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
            Json->SetStringField(TEXT("name"), Node->GetName());
            Json->SetNumberField(TEXT("x"), Node->NodePosX);
            Json->SetNumberField(TEXT("y"), Node->NodePosY);
            if (const UAnimStateNodeBase* State = Cast<UAnimStateNodeBase>(Node))
            {
                Json->SetStringField(TEXT("stateName"), State->GetStateName());
                if (State->GetBoundGraph()) Json->SetStringField(TEXT("boundGraphGuid"), GuidString(State->GetBoundGraph()->GraphGuid));
            }
            if (const UAnimStateTransitionNode* Transition = Cast<UAnimStateTransitionNode>(Node))
            {
                Json->SetStringField(TEXT("previousNodeGuid"), Transition->GetPreviousState() ? GuidString(Transition->GetPreviousState()->NodeGuid) : FString());
                Json->SetStringField(TEXT("nextNodeGuid"), Transition->GetNextState() ? GuidString(Transition->GetNextState()->NodeGuid) : FString());
                Json->SetNumberField(TEXT("priorityOrder"), Transition->PriorityOrder);
                Json->SetNumberField(TEXT("crossfadeDuration"), Transition->CrossfadeDuration);
            }
            TArray<const UEdGraphPin*> SortedPins;
            for (const UEdGraphPin* Pin : Node->Pins) if (Pin) SortedPins.Add(Pin);
            SortedPins.Sort([](const UEdGraphPin& A, const UEdGraphPin& B) { return GuidString(A.PinId) < GuidString(B.PinId); });
            TArray<TSharedPtr<FJsonValue>> Pins;
            for (const UEdGraphPin* Pin : SortedPins) Pins.Add(MakeShared<FJsonValueObject>(PinSnapshot(Pin)));
            Json->SetArrayField(TEXT("pins"), Pins);
            return Json;
        }

        bool JsonContains(const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected, FString& Difference, const FString& Path)
        {
            if (!Actual.IsValid() || !Expected.IsValid() || Actual->Type != Expected->Type)
            {
                Difference = Path + TEXT(": JSON type mismatch");
                return false;
            }
            if (Expected->Type == EJson::Object)
            {
                const TSharedPtr<FJsonObject> A = Actual->AsObject();
                const TSharedPtr<FJsonObject> E = Expected->AsObject();
                for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : E->Values)
                {
                    const TSharedPtr<FJsonValue>* Found = A->Values.Find(Pair.Key);
                    if (!Found)
                    {
                        Difference = Path + TEXT(".") + Pair.Key + TEXT(": missing field");
                        return false;
                    }
                    if (!JsonContains(*Found, Pair.Value, Difference, Path + TEXT(".") + Pair.Key)) return false;
                }
                return true;
            }
            if (Expected->Type == EJson::Array)
            {
                const TArray<TSharedPtr<FJsonValue>>& A = Actual->AsArray();
                const TArray<TSharedPtr<FJsonValue>>& E = Expected->AsArray();
                if (A.Num() != E.Num())
                {
                    Difference = Path + TEXT(": array length mismatch");
                    return false;
                }
                for (int32 Index = 0; Index < E.Num(); ++Index)
                    if (!JsonContains(A[Index], E[Index], Difference, FString::Printf(TEXT("%s[%d]"), *Path, Index))) return false;
                return true;
            }
            FString AText;
            FString EText;
            const TSharedRef<TJsonWriter<>> AWriter = TJsonWriterFactory<>::Create(&AText);
            const TSharedRef<TJsonWriter<>> EWriter = TJsonWriterFactory<>::Create(&EText);
            FJsonSerializer::Serialize(Actual.ToSharedRef(), TEXT(""), AWriter);
            FJsonSerializer::Serialize(Expected.ToSharedRef(), TEXT(""), EWriter);
            if (AText != EText)
            {
                Difference = Path + TEXT(": value mismatch");
                return false;
            }
            return true;
        }

        class FAnimWriteOperation final : public IWriteOperation
        {
        public:
            FAnimWriteOperation(UAnimBlueprint* InBlueprint, const TSharedRef<FJsonObject>& InOperation, const int32 InIndex)
                : Blueprint(InBlueprint), Operation(InOperation), Index(InIndex) {}

            virtual int32 GetOperationIndex() const override { return Index; }

            virtual void GatherPreflight(FPreflightRequest& Request) const override
            {
                if (!Blueprint.IsValid()) return;
                const FString PackageName = Blueprint->GetOutermost()->GetName();
                Request.TargetPackageNames.AddUnique(PackageName);
                Request.CompilePackageNames.AddUnique(PackageName);
                FString Name;
                FString ReferencePath;
                Operation->TryGetStringField(TEXT("operation"), Name);
                if (Name == TEXT("anim.skeleton.set") && Operation->TryGetStringField(TEXT("skeletonPath"), ReferencePath))
                {
                    FTypeReferenceRequirement Requirement;
                    Requirement.ObjectPath = ReferencePath;
                    Requirement.ExpectedClassPath = USkeleton::StaticClass()->GetPathName();
                    Requirement.OperationIndex = Index;
                    Request.TypeReferences.Add(Requirement);
                }
                else if (Name == TEXT("anim.parent.set") && Operation->TryGetStringField(TEXT("parentClassPath"), ReferencePath))
                {
                    FTypeReferenceRequirement Requirement;
                    Requirement.ObjectPath = ReferencePath;
                    Requirement.ExpectedClassPath = UClass::StaticClass()->GetPathName();
                    Requirement.OperationIndex = Index;
                    Request.TypeReferences.Add(Requirement);
                }
            }

            virtual bool Apply(FWriteMutationContext& Context, FWritePipelineError& OutError) override
            {
                UAnimBlueprint* Asset = Blueprint.Get();
                if (!Asset)
                {
                    OutError.Code = TEXT("ANIM_ASSET_UNLOADED");
                    OutError.Message = TEXT("Animation Blueprint was unloaded before mutation.");
                    OutError.OperationIndex = Index;
                    OutError.UECallsite = TEXT("FAnimWriteOperation::Apply");
                    return false;
                }
                FAnimOperationResult Result;
                FAnimOperationError Error;
                if (!FBlueprintAnimOperations::Apply(Asset, Operation, Context, Result, Error, Index))
                {
                    OutError.Code = Error.Code;
                    OutError.Message = Error.Message;
                    OutError.AssetPath = Error.AssetPath;
                    OutError.UECallsite = Error.UECallsite;
                    OutError.OperationIndex = Error.OperationIndex;
                    return false;
                }
                return true;
            }

            virtual bool VerifyInMemory(FWritePipelineError& OutError) const override
            {
                UAnimBlueprint* Asset = Blueprint.Get();
                TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>();
                FAnimOperationError Error;
                if (!Asset || !FBlueprintAnimOperations::Inspect(Asset, Snapshot, Error))
                {
                    OutError.Code = Error.Code.IsEmpty() ? TEXT("ANIM_VERIFY_ASSET_UNLOADED") : Error.Code;
                    OutError.Message = Error.Message.IsEmpty() ? TEXT("Animation Blueprint is unavailable during verification.") : Error.Message;
                    OutError.AssetPath = Error.AssetPath;
                    OutError.UECallsite = Error.UECallsite;
                    OutError.OperationIndex = Index;
                    return false;
                }
                return true;
            }

        private:
            TWeakObjectPtr<UAnimBlueprint> Blueprint;
            TSharedRef<FJsonObject> Operation;
            int32 Index;
        };
    }

    TSharedRef<FJsonObject> FAnimOperationError::ToJson() const
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("code"), Code);
        Json->SetStringField(TEXT("message"), Message);
        Json->SetStringField(TEXT("assetPath"), AssetPath);
        Json->SetStringField(TEXT("graphPath"), GraphPath);
        Json->SetStringField(TEXT("nodeGuid"), NodeGuid);
        Json->SetStringField(TEXT("pinGuid"), PinGuid);
        Json->SetStringField(TEXT("ueCallsite"), UECallsite);
        Json->SetNumberField(TEXT("operationIndex"), OperationIndex);
        TArray<TSharedPtr<FJsonValue>> Values;
        for (const FString& Candidate : Candidates) Values.Add(MakeShared<FJsonValueString>(Candidate));
        Json->SetArrayField(TEXT("candidates"), Values);
        return Json;
    }

    TSharedRef<FJsonObject> FAnimOperationResult::ToJson() const
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetBoolField(TEXT("changed"), bChanged);
        Json->SetStringField(TEXT("assetPath"), AssetPath);
        Json->SetStringField(TEXT("graphPath"), GraphPath);
        Json->SetStringField(TEXT("graphGuid"), GraphGuid);
        TArray<TSharedPtr<FJsonValue>> Nodes;
        for (const FString& Guid : NodeGuids) Nodes.Add(MakeShared<FJsonValueString>(Guid));
        Json->SetArrayField(TEXT("nodeGuids"), Nodes);
        TArray<TSharedPtr<FJsonValue>> Pins;
        for (const FString& Guid : PinGuids) Pins.Add(MakeShared<FJsonValueString>(Guid));
        Json->SetArrayField(TEXT("pinGuids"), Pins);
        TArray<TSharedPtr<FJsonValue>> Packages;
        for (const FString& Package : ImpactPackages) Packages.Add(MakeShared<FJsonValueString>(Package));
        Json->SetArrayField(TEXT("impactPackages"), Packages);
        if (Data.IsValid()) Json->SetObjectField(TEXT("data"), Data.ToSharedRef());
        return Json;
    }

    bool FBlueprintAnimOperations::Apply(UAnimBlueprint* Blueprint, const TSharedRef<FJsonObject>& Operation,
        FWriteMutationContext& Context, FAnimOperationResult& OutResult, FAnimOperationError& OutError,
        const int32 OperationIndex)
    {
        OutResult = FAnimOperationResult();
        OutError = FAnimOperationError();
        if (!IsInGameThread())
        {
            Fail(OutError, TEXT("ANIM_WRONG_THREAD"), TEXT("Animation Blueprint mutations must run on the game thread."),
                TEXT("FBlueprintAnimOperations::Apply"), Blueprint, nullptr, nullptr, nullptr, OperationIndex);
            return false;
        }
        if (!Blueprint)
        {
            Fail(OutError, TEXT("ANIM_BLUEPRINT_REQUIRED"), TEXT("A loaded UAnimBlueprint is required."),
                TEXT("FBlueprintAnimOperations::Apply"), nullptr, nullptr, nullptr, nullptr, OperationIndex);
            return false;
        }
        FString Name;
        FString OperationAssetPath;
        if (!RequiredString(*Operation, TEXT("operation"), Name, OutError, Blueprint, OperationIndex)
            || !RequiredString(*Operation, TEXT("assetPath"), OperationAssetPath, OutError, Blueprint, OperationIndex)) return false;
        if (OperationAssetPath != Blueprint->GetPathName())
        {
            Fail(OutError, TEXT("ANIM_ASSET_PATH_MISMATCH"),
                TEXT("'assetPath' does not identify the supplied Animation Blueprint."),
                TEXT("FBlueprintAnimOperations::Apply"), Blueprint, nullptr, nullptr, nullptr, OperationIndex);
            return false;
        }
        FWritePipelineError ModifyError;
        if (!Context.Modify(Blueprint, ModifyError))
        {
            Fail(OutError, *ModifyError.Code, ModifyError.Message, *ModifyError.UECallsite, Blueprint,
                nullptr, nullptr, nullptr, OperationIndex);
            return false;
        }
        Context.MarkPackageChanged(Blueprint->GetOutermost());

        if (Name == TEXT("anim.skeleton.set"))
        {
            FString Path;
            if (!RequiredString(*Operation, TEXT("skeletonPath"), Path, OutError, Blueprint, OperationIndex)) return false;
            USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, *Path, nullptr, LOAD_NoWarn);
            if (!Skeleton)
            {
                Fail(OutError, TEXT("ANIM_SKELETON_NOT_FOUND"), FString::Printf(TEXT("Skeleton '%s' was not found."), *Path),
                    TEXT("LoadObject<USkeleton>"), Blueprint, nullptr, nullptr, nullptr, OperationIndex);
                return false;
            }
            if (UAnimBlueprint* RootParent = UAnimBlueprint::FindRootAnimBlueprint(Blueprint))
            {
                if (RootParent != Blueprint && RootParent->TargetSkeleton && RootParent->TargetSkeleton != Skeleton)
                {
                    Fail(OutError, TEXT("ANIM_SKELETON_PARENT_MISMATCH"),
                        TEXT("A derived Animation Blueprint must use its root parent Animation Blueprint's skeleton."),
                        TEXT("UAnimBlueprint::FindRootAnimBlueprint"), Blueprint, nullptr, nullptr, nullptr, OperationIndex);
                    return false;
                }
            }
            Blueprint->TargetSkeleton = Skeleton;
            FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
            FillResult(Blueprint, nullptr, OutResult);
            return true;
        }
        if (Name == TEXT("anim.parent.set"))
        {
            FString Path;
            if (!RequiredString(*Operation, TEXT("parentClassPath"), Path, OutError, Blueprint, OperationIndex)) return false;
            UClass* Parent = LoadObject<UClass>(nullptr, *Path, nullptr, LOAD_NoWarn);
            if (!Parent || !Parent->IsChildOf(UAnimInstance::StaticClass()) || Parent == Blueprint->GeneratedClass)
            {
                Fail(OutError, TEXT("ANIM_PARENT_INVALID"),
                    TEXT("parentClassPath must resolve to an AnimInstance class that is not this Blueprint's generated class."),
                    TEXT("UClass::IsChildOf"), Blueprint, nullptr, nullptr, nullptr, OperationIndex);
                return false;
            }
            for (UClass* Current = Parent; Current; Current = Current->GetSuperClass())
            {
                if (Current == Blueprint->GeneratedClass)
                {
                    Fail(OutError, TEXT("ANIM_PARENT_CYCLE"), TEXT("The requested parent would create an inheritance cycle."),
                        TEXT("UClass::GetSuperClass"), Blueprint, nullptr, nullptr, nullptr, OperationIndex);
                    return false;
                }
            }
            if (const UAnimBlueprint* ParentBlueprint = Cast<UAnimBlueprint>(Parent->ClassGeneratedBy))
            {
                if (Blueprint->TargetSkeleton && ParentBlueprint->TargetSkeleton
                    && Blueprint->TargetSkeleton != ParentBlueprint->TargetSkeleton)
                {
                    Fail(OutError, TEXT("ANIM_PARENT_SKELETON_MISMATCH"),
                        TEXT("The requested parent Animation Blueprint uses a different skeleton."),
                        TEXT("UClass::ClassGeneratedBy"), Blueprint, nullptr, nullptr, nullptr, OperationIndex);
                    return false;
                }
            }
            Blueprint->ParentClass = Parent;
            if (UAnimBlueprint* RootParent = UAnimBlueprint::FindRootAnimBlueprint(Blueprint))
                Blueprint->TargetSkeleton = RootParent->TargetSkeleton;
            FBlueprintEditorUtils::UpdateOutOfDateAnimBlueprints(Blueprint);
            FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
            FillResult(Blueprint, nullptr, OutResult);
            return true;
        }
        if (Name.StartsWith(TEXT("anim.variable.")))
            return ApplyVariable(Blueprint, *Operation, Name, OutResult, OutError, OperationIndex);

        UEdGraph* Graph = FindGraph(Blueprint, *Operation, OutError, OperationIndex);
        if (!Graph) return false;
        if (!Context.Modify(Graph, ModifyError))
        {
            Fail(OutError, *ModifyError.Code, ModifyError.Message, *ModifyError.UECallsite, Blueprint,
                Graph, nullptr, nullptr, OperationIndex);
            return false;
        }
        FString ExistingNodeGuidText;
        FGuid ExistingNodeGuid;
        if (Operation->TryGetStringField(TEXT("nodeGuid"), ExistingNodeGuidText)
            && FGuid::Parse(ExistingNodeGuidText, ExistingNodeGuid) && ExistingNodeGuid.IsValid())
        {
            UEdGraphNode* ExistingNode = FindNode(Graph, ExistingNodeGuid, OutError, Blueprint, OperationIndex);
            if (!ExistingNode) return false;
            if (!Context.Modify(ExistingNode, ModifyError))
            {
                Fail(OutError, *ModifyError.Code, ModifyError.Message, *ModifyError.UECallsite, Blueprint,
                    Graph, ExistingNode, nullptr, OperationIndex);
                return false;
            }
        }

        if (Name == TEXT("anim.node.spawn"))
        {
            if (!RequireSchema(Graph, UAnimationGraphSchema::StaticClass(), OutError, Blueprint, OperationIndex,
                TEXT("FBlueprintGraphActionCatalog::Spawn"))) return false;
            return SpawnActionNode(Blueprint, Graph, *Operation, OutResult, OutError, OperationIndex);
        }
        if (Name == TEXT("anim.stateMachine.add"))
        {
            if (!RequireSchema(Graph, UAnimationGraphSchema::StaticClass(), OutError, Blueprint, OperationIndex,
                TEXT("FBlueprintGraphActionCatalog::Spawn"))) return false;
            if (!SpawnActionNode(Blueprint, Graph, *Operation, OutResult, OutError, OperationIndex)) return false;
            UEdGraphNode* Spawned = nullptr;
            FGuid Guid;
            FGuid::Parse(OutResult.NodeGuids[0], Guid);
            Spawned = FindNode(Graph, Guid, OutError, Blueprint, OperationIndex);
            UAnimGraphNode_StateMachineBase* Machine = Cast<UAnimGraphNode_StateMachineBase>(Spawned);
            if (!Machine || !Machine->EditorStateMachineGraph)
            {
                if (Spawned) FBlueprintEditorUtils::RemoveNode(Blueprint, Spawned, true);
                Fail(OutError, TEXT("ANIM_STATE_MACHINE_ACTION_REQUIRED"),
                    TEXT("actionId must resolve to an Animation State Machine node spawner."),
                    TEXT("Cast<UAnimGraphNode_StateMachineBase>"), Blueprint, Graph, Spawned, nullptr, OperationIndex);
                return false;
            }
            FString MachineName;
            if (Operation->TryGetStringField(TEXT("name"), MachineName) && !MachineName.IsEmpty())
            {
                FBlueprintEditorUtils::RenameGraph(Machine->EditorStateMachineGraph, MachineName);
                if (Machine->EditorStateMachineGraph->GetName() != MachineName)
                {
                    FBlueprintEditorUtils::RemoveNode(Blueprint, Machine, true);
                    Fail(OutError, TEXT("ANIM_GRAPH_NAME_CONFLICT"),
                        TEXT("UE could not assign the requested state machine name exactly."),
                        TEXT("FBlueprintEditorUtils::RenameGraph"), Blueprint, Graph, Machine, nullptr, OperationIndex);
                    return false;
                }
            }
            OutResult.Data = MakeShared<FJsonObject>();
            OutResult.Data->SetStringField(TEXT("stateMachineGraphGuid"), GuidString(Machine->EditorStateMachineGraph->GraphGuid));
            OutResult.Data->SetStringField(TEXT("stateMachineGraphPath"), Machine->EditorStateMachineGraph->GetPathName());
            return true;
        }
        if (Name == TEXT("anim.stateMachine.rename"))
        {
            if (!RequireSchema(Graph, UAnimationGraphSchema::StaticClass(), OutError, Blueprint, OperationIndex,
                TEXT("anim.stateMachine.rename"))) return false;
            FGuid Guid;
            FString MachineName;
            if (!RequiredGuid(*Operation, TEXT("nodeGuid"), Guid, OutError, Blueprint, OperationIndex)
                || !RequiredString(*Operation, TEXT("name"), MachineName, OutError, Blueprint, OperationIndex)) return false;
            UAnimGraphNode_StateMachineBase* Machine = Cast<UAnimGraphNode_StateMachineBase>(
                FindNode(Graph, Guid, OutError, Blueprint, OperationIndex));
            if (!Machine || !Machine->EditorStateMachineGraph)
            {
                if (!OutError.IsSet()) Fail(OutError, TEXT("ANIM_STATE_MACHINE_NODE_REQUIRED"),
                    TEXT("nodeGuid must identify an Animation State Machine node."),
                    TEXT("Cast<UAnimGraphNode_StateMachineBase>"), Blueprint, Graph, Machine, nullptr, OperationIndex);
                return false;
            }
            FBlueprintEditorUtils::RenameGraph(Machine->EditorStateMachineGraph, MachineName);
            if (Machine->EditorStateMachineGraph->GetName() != MachineName)
            {
                Fail(OutError, TEXT("ANIM_GRAPH_NAME_CONFLICT"), TEXT("UE could not assign the requested state machine name exactly."),
                    TEXT("FBlueprintEditorUtils::RenameGraph"), Blueprint, Graph, Machine, nullptr, OperationIndex);
                return false;
            }
            FillResult(Blueprint, Graph, OutResult, true);
            OutResult.NodeGuids.Add(GuidString(Machine->NodeGuid));
            return true;
        }
        if (Name == TEXT("anim.stateMachine.entry.set"))
            return SetStateMachineEntry(Blueprint, Graph, *Operation, OutResult, OutError, OperationIndex);
        if (Name == TEXT("anim.state.add")) return SpawnStateLike(Blueprint, Graph, *Operation, false, OutResult, OutError, OperationIndex);
        if (Name == TEXT("anim.conduit.add")) return SpawnStateLike(Blueprint, Graph, *Operation, true, OutResult, OutError, OperationIndex);
        if (Name == TEXT("anim.transition.add")) return AddTransition(Blueprint, Graph, *Operation, OutResult, OutError, OperationIndex);
        if (Name == TEXT("anim.state.rename") || Name == TEXT("anim.conduit.rename"))
        {
            if (!RequireSchema(Graph, UAnimationStateMachineSchema::StaticClass(), OutError, Blueprint, OperationIndex,
                TEXT("anim.state.rename"))) return false;
            return RenameStateLike(Blueprint, Graph, *Operation, OutResult, OutError, OperationIndex);
        }
        if (Name == TEXT("anim.poseLink.connect")) return ConnectPose(Blueprint, Graph, *Operation, true, OutResult, OutError, OperationIndex);
        if (Name == TEXT("anim.poseLink.disconnect")) return ConnectPose(Blueprint, Graph, *Operation, false, OutResult, OutError, OperationIndex);
        if (Name == TEXT("anim.rule.node.spawn"))
        {
            if (!RequireSchema(Graph, UAnimationTransitionSchema::StaticClass(), OutError, Blueprint, OperationIndex,
                TEXT("FBlueprintGraphActionCatalog::Spawn"))) return false;
            return SpawnActionNode(Blueprint, Graph, *Operation, OutResult, OutError, OperationIndex);
        }
        if (Name == TEXT("anim.rule.link.connect") || Name == TEXT("anim.rule.link.disconnect"))
        {
            if (!RequireSchema(Graph, UAnimationTransitionSchema::StaticClass(), OutError, Blueprint, OperationIndex,
                TEXT("anim.rule.link.connect"))) return false;
            return ConnectSchemaPins(Blueprint, Graph, *Operation, Name == TEXT("anim.rule.link.connect"), OutResult, OutError, OperationIndex);
        }
        if (Name == TEXT("anim.event.node.spawn"))
        {
            if (!RequireEventGraph(Blueprint, Graph, OutError, OperationIndex)) return false;
            return SpawnActionNode(Blueprint, Graph, *Operation, OutResult, OutError, OperationIndex);
        }
        if (Name == TEXT("anim.node.move"))
        {
            if (!RequireSchema(Graph, UAnimationGraphSchema::StaticClass(), OutError, Blueprint, OperationIndex,
                TEXT("anim.node.move"))) return false;
            return MoveNode(Blueprint, Graph, *Operation, OutResult, OutError, OperationIndex);
        }
        if (Name == TEXT("anim.node.property.set"))
        {
            if (!RequireSchema(Graph, UAnimationGraphSchema::StaticClass(), OutError, Blueprint, OperationIndex,
                TEXT("anim.node.property.set"))) return false;
            return SetNodeProperty(Blueprint, Graph, *Operation, OutResult, OutError, OperationIndex);
        }
        if (Name == TEXT("anim.transition.property.set"))
        {
            if (!RequireSchema(Graph, UAnimationStateMachineSchema::StaticClass(), OutError, Blueprint, OperationIndex,
                TEXT("anim.transition.property.set"))) return false;
            return SetNodeProperty(Blueprint, Graph, *Operation, OutResult, OutError, OperationIndex, UAnimStateTransitionNode::StaticClass());
        }
        if (Name == TEXT("anim.rule.node.property.set"))
        {
            if (!RequireSchema(Graph, UAnimationTransitionSchema::StaticClass(), OutError, Blueprint, OperationIndex,
                TEXT("anim.rule.node.property.set"))) return false;
            return SetNodeProperty(Blueprint, Graph, *Operation, OutResult, OutError, OperationIndex);
        }
        if (Name == TEXT("anim.event.node.property.set"))
        {
            if (!RequireEventGraph(Blueprint, Graph, OutError, OperationIndex)) return false;
            return SetNodeProperty(Blueprint, Graph, *Operation, OutResult, OutError, OperationIndex);
        }
        if (Name == TEXT("anim.event.link.connect") || Name == TEXT("anim.event.link.disconnect"))
        {
            if (!RequireEventGraph(Blueprint, Graph, OutError, OperationIndex)) return false;
            return ConnectSchemaPins(Blueprint, Graph, *Operation, Name == TEXT("anim.event.link.connect"), OutResult, OutError, OperationIndex);
        }
        if (Name == TEXT("anim.node.delete") || Name == TEXT("anim.stateMachine.remove"))
        {
            if (!RequireSchema(Graph, UAnimationGraphSchema::StaticClass(), OutError, Blueprint, OperationIndex,
                TEXT("anim.node.delete"))) return false;
            return RemoveNode(Blueprint, Graph, *Operation, OutResult, OutError, OperationIndex,
                Name == TEXT("anim.stateMachine.remove") ? UAnimGraphNode_StateMachineBase::StaticClass() : nullptr);
        }
        if (Name == TEXT("anim.state.remove") || Name == TEXT("anim.conduit.remove") || Name == TEXT("anim.transition.remove"))
        {
            if (!RequireSchema(Graph, UAnimationStateMachineSchema::StaticClass(), OutError, Blueprint, OperationIndex,
                TEXT("anim.state.remove"))) return false;
            UClass* RequiredClass = Name == TEXT("anim.state.remove") ? UAnimStateNode::StaticClass()
                : (Name == TEXT("anim.conduit.remove") ? UAnimStateConduitNode::StaticClass()
                                                       : UAnimStateTransitionNode::StaticClass());
            return RemoveNode(Blueprint, Graph, *Operation, OutResult, OutError, OperationIndex, RequiredClass);
        }
        if (Name == TEXT("anim.rule.node.delete"))
        {
            if (!RequireSchema(Graph, UAnimationTransitionSchema::StaticClass(), OutError, Blueprint, OperationIndex,
                TEXT("anim.rule.node.delete"))) return false;
            return RemoveNode(Blueprint, Graph, *Operation, OutResult, OutError, OperationIndex);
        }
        if (Name == TEXT("anim.event.node.delete"))
        {
            if (!RequireEventGraph(Blueprint, Graph, OutError, OperationIndex)) return false;
            return RemoveNode(Blueprint, Graph, *Operation, OutResult, OutError, OperationIndex);
        }

        Fail(OutError, TEXT("ANIM_OPERATION_UNKNOWN"), FString::Printf(TEXT("Unknown Animation Blueprint operation '%s'."), *Name),
            TEXT("FBlueprintAnimOperations::Apply"), Blueprint, Graph, nullptr, nullptr, OperationIndex);
        return false;
    }

    bool FBlueprintAnimOperations::Inspect(UAnimBlueprint* Blueprint, TSharedRef<FJsonObject>& OutSnapshot,
        FAnimOperationError& OutError)
    {
        OutError = FAnimOperationError();
        if (!IsInGameThread())
        {
            Fail(OutError, TEXT("ANIM_WRONG_THREAD"), TEXT("Animation Blueprint inspection must run on the game thread."),
                TEXT("FBlueprintAnimOperations::Inspect"), Blueprint);
            return false;
        }
        if (!Blueprint)
        {
            Fail(OutError, TEXT("ANIM_BLUEPRINT_REQUIRED"), TEXT("A loaded UAnimBlueprint is required."),
                TEXT("FBlueprintAnimOperations::Inspect"));
            return false;
        }
        TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>();
        Snapshot->SetStringField(TEXT("assetPath"), Blueprint->GetPathName());
        Snapshot->SetStringField(TEXT("packageName"), Blueprint->GetOutermost()->GetName());
        Snapshot->SetStringField(TEXT("skeletonPath"), Blueprint->TargetSkeleton ? Blueprint->TargetSkeleton->GetPathName() : FString());
        Snapshot->SetStringField(TEXT("parentClassPath"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : FString());

        TArray<UEdGraph*> Graphs;
        Blueprint->GetAllGraphs(Graphs);
        Graphs.RemoveAll([](const UEdGraph* Graph) { return Graph == nullptr; });
        Graphs.Sort([](const UEdGraph& A, const UEdGraph& B) { return GuidString(A.GraphGuid) < GuidString(B.GraphGuid); });
        TArray<TSharedPtr<FJsonValue>> GraphValues;
        for (const UEdGraph* Graph : Graphs)
        {
            TSharedRef<FJsonObject> GraphJson = MakeShared<FJsonObject>();
            GraphJson->SetStringField(TEXT("guid"), GuidString(Graph->GraphGuid));
            GraphJson->SetStringField(TEXT("name"), Graph->GetName());
            GraphJson->SetStringField(TEXT("path"), Graph->GetPathName());
            GraphJson->SetStringField(TEXT("schema"), Graph->GetSchema() ? Graph->GetSchema()->GetClass()->GetPathName() : FString());
            GraphJson->SetStringField(TEXT("kind"), SchemaKind(Graph));
            TArray<const UEdGraphNode*> Nodes;
            for (const UEdGraphNode* Node : Graph->Nodes) if (Node) Nodes.Add(Node);
            Nodes.Sort([](const UEdGraphNode& A, const UEdGraphNode& B) { return GuidString(A.NodeGuid) < GuidString(B.NodeGuid); });
            TArray<TSharedPtr<FJsonValue>> NodeValues;
            for (const UEdGraphNode* Node : Nodes) NodeValues.Add(MakeShared<FJsonValueObject>(NodeSnapshot(Node)));
            GraphJson->SetArrayField(TEXT("nodes"), NodeValues);
            GraphValues.Add(MakeShared<FJsonValueObject>(GraphJson));
        }
        Snapshot->SetArrayField(TEXT("graphs"), GraphValues);

        TArray<TSharedPtr<FJsonValue>> Variables;
        TArray<FBPVariableDescription> SortedVariables = Blueprint->NewVariables;
        SortedVariables.Sort([](const FBPVariableDescription& A, const FBPVariableDescription& B)
            { return A.VarGuid.ToString() < B.VarGuid.ToString(); });
        for (const FBPVariableDescription& Variable : SortedVariables)
        {
            TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
            Json->SetStringField(TEXT("guid"), GuidString(Variable.VarGuid));
            Json->SetStringField(TEXT("name"), Variable.VarName.ToString());
            Json->SetObjectField(TEXT("type"), FBlueprintTypeSystem::PinTypeToJson(Variable.VarType));
            Json->SetStringField(TEXT("category"), Variable.Category.ToString());
            Variables.Add(MakeShared<FJsonValueObject>(Json));
        }
        Snapshot->SetArrayField(TEXT("variables"), Variables);

        FString Canonical;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Canonical);
        FJsonSerializer::Serialize(Snapshot, Writer);
        const FTCHARToUTF8 Utf8(*Canonical);
        FSHAHash Hash;
        FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Hash.Hash);
        Snapshot->SetStringField(TEXT("structureHash"), Hash.ToString().ToLower());
        OutSnapshot = Snapshot;
        return true;
    }

    bool FBlueprintAnimOperations::VerifySnapshot(UAnimBlueprint* Blueprint, const TSharedRef<FJsonObject>& Expected,
        TSharedRef<FJsonObject>& OutActual, FAnimOperationError& OutError)
    {
        if (!Inspect(Blueprint, OutActual, OutError)) return false;
        FString Difference;
        if (!JsonContains(MakeShared<FJsonValueObject>(OutActual), MakeShared<FJsonValueObject>(Expected), Difference, TEXT("snapshot")))
        {
            Fail(OutError, TEXT("ANIM_SNAPSHOT_MISMATCH"), Difference, TEXT("FBlueprintAnimOperations::VerifySnapshot"), Blueprint);
            return false;
        }
        return true;
    }

    bool FBlueprintAnimOperations::BuildWriteRequest(UAnimBlueprint* Blueprint,
        const TArray<TSharedRef<FJsonObject>>& Operations, const FString& RequestId,
        const TFunction<bool(const FString&, FString&, FString&)>& StateHashResolver,
        FWritePipelineRequest& OutRequest, FAnimOperationError& OutError)
    {
        OutError = FAnimOperationError();
        if (!Blueprint || RequestId.TrimStartAndEnd().IsEmpty() || Operations.Num() == 0)
        {
            Fail(OutError, TEXT("ANIM_WRITE_REQUEST_INVALID"),
                TEXT("Blueprint, non-empty requestId, and at least one operation are required."),
                TEXT("FBlueprintAnimOperations::BuildWriteRequest"), Blueprint);
            return false;
        }
        OutRequest = FWritePipelineRequest();
        OutRequest.RequestId = RequestId;
        OutRequest.TransactionDescription = TEXT("Codex Animation Blueprint automation");
        OutRequest.StateHashResolver = StateHashResolver;
        OutRequest.Preflight.StateHashResolver = StateHashResolver;
        const FString PackageName = Blueprint->GetOutermost()->GetName();
        OutRequest.Preflight.TargetPackageNames.Add(PackageName);
        OutRequest.Preflight.CompilePackageNames.Add(PackageName);
        if (StateHashResolver)
        {
            FString Hash;
            FString HashError;
            if (!StateHashResolver(PackageName, Hash, HashError))
            {
                Fail(OutError, TEXT("ANIM_STATE_HASH_FAILED"), HashError,
                    TEXT("StateHashResolver"), Blueprint);
                return false;
            }
            OutRequest.Preflight.ExpectedStateHashes.Add(PackageName, Hash);
        }
        for (int32 Index = 0; Index < Operations.Num(); ++Index)
            OutRequest.Operations.Add(MakeShared<FAnimWriteOperation>(Blueprint, Operations[Index], Index));
        return true;
    }

    FWritePipelineResult FBlueprintAnimOperations::Execute(UAnimBlueprint* Blueprint,
        const TArray<TSharedRef<FJsonObject>>& Operations, const FString& RequestId,
        const TFunction<bool(const FString&, FString&, FString&)>& StateHashResolver,
        const FWritePipelineProgress& Progress, FAnimOperationError& OutError)
    {
        FWritePipelineRequest Request;
        if (!BuildWriteRequest(Blueprint, Operations, RequestId, StateHashResolver, Request, OutError))
        {
            FWritePipelineResult Result;
            Result.Error.Code = OutError.Code;
            Result.Error.Message = OutError.Message;
            Result.Error.AssetPath = OutError.AssetPath;
            Result.Error.UECallsite = OutError.UECallsite;
            Result.Error.OperationIndex = OutError.OperationIndex;
            return Result;
        }
        return FWritePipeline::Execute(Request, Progress);
    }
}
