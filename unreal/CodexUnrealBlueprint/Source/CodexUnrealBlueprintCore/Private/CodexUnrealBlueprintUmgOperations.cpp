#include "CodexUnrealBlueprintUmgOperations.h"

#include "Animation/WidgetAnimation.h"
#include "Animation/WidgetAnimationBinding.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetNavigation.h"
#include "Blueprint/WidgetTree.h"
#include "Components/NamedSlotInterface.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "EdGraphSchema_K2_Actions.h"
#include "K2Node_ComponentBoundEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/SecureHash.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"
#include "Channels/MovieSceneBoolChannel.h"
#include "Channels/MovieSceneByteChannel.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Channels/MovieSceneIntegerChannel.h"
#include "Channels/MovieSceneStringChannel.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "CodexUnrealBlueprintTypeSystem.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "WidgetBlueprint.h"

namespace CodexUnrealBlueprint
{
    namespace
    {
        FString CodexUmgAssetPath(const UWidgetBlueprint* Blueprint)
        {
            return Blueprint ? Blueprint->GetPathName() : FString();
        }

        FUmgOperationError CodexUmgError(const FString& Code, const FString& Message, const UWidgetBlueprint* Blueprint,
            const FString& Callsite, const int32 OperationIndex, const FString& WidgetPath = FString(),
            const FString& AnimationName = FString(), const TArray<FString>& Details = TArray<FString>())
        {
            FUmgOperationError Result;
            Result.Code = Code;
            Result.Message = Message;
            Result.AssetPath = CodexUmgAssetPath(Blueprint);
            Result.WidgetPath = WidgetPath;
            Result.AnimationName = AnimationName;
            Result.UECallsite = Callsite;
            Result.OperationIndex = OperationIndex;
            Result.Details = Details;
            return Result;
        }

        bool CodexUmgRequireString(const TSharedRef<FJsonObject>& Json, const TCHAR* Field, FString& Out,
            FUmgOperationError& OutError, UWidgetBlueprint* Blueprint, int32 Index, const FString& Callsite)
        {
            if (!Json->TryGetStringField(Field, Out) || Out.TrimStartAndEnd().IsEmpty())
            {
                OutError = CodexUmgError(TEXT("UmgInvalidArgument"), FString::Printf(TEXT("'%s' must be a non-empty string."), Field),
                    Blueprint, Callsite, Index);
                return false;
            }
            return true;
        }

        bool CodexUmgRequireInteger(const TSharedRef<FJsonObject>& Json, const TCHAR* Field, int32& Out,
            FUmgOperationError& OutError, UWidgetBlueprint* Blueprint, int32 Index, const FString& Callsite)
        {
            double Number = 0.0;
            if (!Json->TryGetNumberField(Field, Number) || Number != FMath::RoundToDouble(Number)
                || Number < static_cast<double>(MIN_int32) || Number > static_cast<double>(MAX_int32))
            {
                OutError = CodexUmgError(TEXT("UmgInvalidArgument"), FString::Printf(TEXT("'%s' must be an int32."), Field),
                    Blueprint, Callsite, Index);
                return false;
            }
            Out = static_cast<int32>(Number);
            return true;
        }

        bool CodexUmgRequireValue(const TSharedRef<FJsonObject>& Json, const TCHAR* Field, TSharedPtr<FJsonValue>& Out,
            FUmgOperationError& OutError, UWidgetBlueprint* Blueprint, int32 Index, const FString& Callsite)
        {
            Out = Json->TryGetField(Field);
            if (!Out.IsValid())
            {
                OutError = CodexUmgError(TEXT("UmgInvalidArgument"), FString::Printf(TEXT("'%s' is required."), Field),
                    Blueprint, Callsite, Index);
                return false;
            }
            return true;
        }

        bool CodexUmgModifyObject(FWriteMutationContext& Context, UObject* Object, FWritePipelineError& ModifyError,
            FUmgOperationError& OutError, UWidgetBlueprint* Blueprint, const int32 Index,
            const FString& WidgetPath = FString(), const FString& AnimationName = FString())
        {
            if (Context.Modify(Object, ModifyError)) return true;
            OutError = CodexUmgError(ModifyError.Code, ModifyError.Message, Blueprint, ModifyError.UECallsite,
                Index, WidgetPath, AnimationName);
            return false;
        }

        UWidget* CodexUmgFindWidget(UWidgetBlueprint* Blueprint, const FString& Name, FUmgOperationError& OutError,
            int32 Index, const FString& Callsite)
        {
            if (!Blueprint || !Blueprint->WidgetTree)
            {
                OutError = CodexUmgError(TEXT("UmgInvalidBlueprint"), TEXT("Widget Blueprint has no WidgetTree."), Blueprint, Callsite, Index);
                return nullptr;
            }
            UWidget* Widget = Blueprint->WidgetTree->FindWidget(FName(*Name));
            if (!Widget)
            {
                OutError = CodexUmgError(TEXT("UmgWidgetNotFound"), FString::Printf(TEXT("Widget '%s' was not found."), *Name),
                    Blueprint, Callsite, Index, Name);
            }
            return Widget;
        }

        UWidgetAnimation* CodexUmgFindAnimation(UWidgetBlueprint* Blueprint, const FString& Name, FUmgOperationError& OutError,
            int32 Index, const FString& Callsite)
        {
            if (Blueprint)
            {
                for (UWidgetAnimation* Animation : Blueprint->Animations)
                {
                    if (Animation && (Animation->GetName() == Name || Animation->GetDisplayLabel() == Name)) return Animation;
                }
            }
            OutError = CodexUmgError(TEXT("UmgAnimationNotFound"), FString::Printf(TEXT("Animation '%s' was not found."), *Name),
                Blueprint, Callsite, Index, FString(), Name);
            return nullptr;
        }

        bool CodexUmgParseGuid(const FString& Text, FGuid& OutGuid)
        {
            return FGuid::Parse(Text, OutGuid) && OutGuid.IsValid();
        }

        FWidgetAnimationBinding* CodexUmgFindAnimationBinding(UWidgetAnimation* Animation, const FGuid& Guid)
        {
            return Animation ? Animation->AnimationBindings.FindByPredicate(
                [&Guid](const FWidgetAnimationBinding& Item) { return Item.AnimationGuid == Guid; }) : nullptr;
        }

        bool CodexUmgFindNamedSlotOwner(UWidgetBlueprint* Blueprint, UWidget* Child, UWidget*& OutOwner, FName& OutSlot)
        {
            OutOwner = nullptr;
            OutSlot = NAME_None;
            if (!Blueprint || !Blueprint->WidgetTree || !Child) return false;
            bool bFound = false;
            Blueprint->WidgetTree->ForEachWidget([&](UWidget* Candidate)
            {
                if (bFound || !Candidate || !Candidate->GetClass()->ImplementsInterface(UNamedSlotInterface::StaticClass())) return;
                INamedSlotInterface* Interface = Cast<INamedSlotInterface>(Candidate);
                TArray<FName> Slots;
                Interface->GetSlotNames(Slots);
                for (const FName Slot : Slots)
                {
                    if (Interface->GetContentForSlot(Slot) == Child)
                    {
                        OutOwner = Candidate;
                        OutSlot = Slot;
                        bFound = true;
                        return;
                    }
                }
            });
            return bFound;
        }

        bool CodexUmgDetachWidget(UWidgetBlueprint* Blueprint, UWidget* Widget)
        {
            if (!Blueprint || !Blueprint->WidgetTree || !Widget) return false;
            int32 ChildIndex = INDEX_NONE;
            if (UPanelWidget* Parent = UWidgetTree::FindWidgetParent(Widget, ChildIndex)) return Parent->RemoveChild(Widget);
            UWidget* Owner = nullptr;
            FName Slot;
            if (CodexUmgFindNamedSlotOwner(Blueprint, Widget, Owner, Slot))
            {
                Cast<INamedSlotInterface>(Owner)->SetContentForSlot(Slot, nullptr);
                return true;
            }
            if (Blueprint->WidgetTree->RootWidget == Widget)
            {
                Blueprint->WidgetTree->RootWidget = nullptr;
                return true;
            }
            return Widget->Slot == nullptr;
        }

        bool CodexUmgIsDescendant(UWidget* CandidateParent, UWidget* Widget)
        {
            if (!CandidateParent || !Widget) return false;
            if (CandidateParent == Widget) return true;
            bool bFound = false;
            UWidgetTree::ForWidgetAndChildren(Widget, [&](UWidget* Child)
            {
                if (Child == CandidateParent) bFound = true;
            });
            return bFound;
        }

        bool CodexUmgAttachWidget(UWidgetBlueprint* Blueprint, UWidget* Widget, const TSharedRef<FJsonObject>& Operation,
            FUmgOperationError& OutError, int32 Index, const FString& Callsite)
        {
            FString ParentName;
            FString NamedSlot;
            const bool bHasParent = Operation->TryGetStringField(TEXT("parent"), ParentName) && !ParentName.IsEmpty();
            const bool bHasNamedSlot = Operation->TryGetStringField(TEXT("namedSlot"), NamedSlot) && !NamedSlot.IsEmpty();
            if (!bHasParent)
            {
                if (bHasNamedSlot)
                {
                    OutError = CodexUmgError(TEXT("UmgInvalidParent"), TEXT("'namedSlot' requires 'parent'."), Blueprint, Callsite, Index, Widget->GetName());
                    return false;
                }
                if (Blueprint->WidgetTree->RootWidget && Blueprint->WidgetTree->RootWidget != Widget)
                {
                    OutError = CodexUmgError(TEXT("UmgRootAlreadyExists"), TEXT("WidgetTree already has a root widget."), Blueprint, Callsite, Index, Widget->GetName());
                    return false;
                }
                Blueprint->WidgetTree->RootWidget = Widget;
                return true;
            }

            UWidget* Parent = CodexUmgFindWidget(Blueprint, ParentName, OutError, Index, Callsite);
            if (!Parent) return false;
            if (CodexUmgIsDescendant(Parent, Widget))
            {
                OutError = CodexUmgError(TEXT("UmgHierarchyCycle"), TEXT("The requested parent is the widget itself or its descendant."),
                    Blueprint, Callsite, Index, Widget->GetName());
                return false;
            }
            if (bHasNamedSlot)
            {
                if (!Parent->GetClass()->ImplementsInterface(UNamedSlotInterface::StaticClass()))
                {
                    OutError = CodexUmgError(TEXT("UmgNamedSlotUnsupported"), FString::Printf(TEXT("Widget '%s' has no named slots."), *ParentName),
                        Blueprint, Callsite, Index, ParentName);
                    return false;
                }
                INamedSlotInterface* Interface = Cast<INamedSlotInterface>(Parent);
                TArray<FName> Slots;
                Interface->GetSlotNames(Slots);
                const FName SlotName(*NamedSlot);
                if (!Slots.Contains(SlotName))
                {
                    TArray<FString> Candidates;
                    for (const FName Candidate : Slots) Candidates.Add(Candidate.ToString());
                    OutError = CodexUmgError(TEXT("UmgNamedSlotNotFound"), FString::Printf(TEXT("Named slot '%s' was not found."), *NamedSlot),
                        Blueprint, Callsite, Index, ParentName, FString(), Candidates);
                    return false;
                }
                UWidget* Existing = Interface->GetContentForSlot(SlotName);
                if (Existing && Existing != Widget)
                {
                    OutError = CodexUmgError(TEXT("UmgNamedSlotOccupied"), FString::Printf(TEXT("Named slot '%s' already contains '%s'."),
                        *NamedSlot, *Existing->GetName()), Blueprint, Callsite, Index, ParentName);
                    return false;
                }
                Interface->SetContentForSlot(SlotName, Widget);
                return true;
            }

            UPanelWidget* Panel = Cast<UPanelWidget>(Parent);
            if (!Panel)
            {
                OutError = CodexUmgError(TEXT("UmgParentIsNotPanel"), FString::Printf(TEXT("Widget '%s' is not a panel."), *ParentName),
                    Blueprint, Callsite, Index, ParentName);
                return false;
            }
            if (!Panel->CanAddMoreChildren())
            {
                OutError = CodexUmgError(TEXT("UmgPanelFull"), FString::Printf(TEXT("Panel '%s' cannot accept another child."), *ParentName),
                    Blueprint, Callsite, Index, ParentName);
                return false;
            }
            int32 ChildIndex = Panel->GetChildrenCount();
            if (Operation->HasField(TEXT("childIndex")))
            {
                if (!CodexUmgRequireInteger(Operation, TEXT("childIndex"), ChildIndex, OutError, Blueprint, Index, Callsite)) return false;
                if (ChildIndex < 0 || ChildIndex > Panel->GetChildrenCount())
                {
                    OutError = CodexUmgError(TEXT("UmgChildIndexOutOfRange"), TEXT("'childIndex' is outside the parent panel."),
                        Blueprint, Callsite, Index, ParentName);
                    return false;
                }
            }
#if WITH_EDITOR
            UPanelSlot* Slot = Panel->InsertChildAt(ChildIndex, Widget);
#else
            UPanelSlot* Slot = Panel->AddChild(Widget);
#endif
            if (!Slot)
            {
                OutError = CodexUmgError(TEXT("UmgAttachFailed"), TEXT("UE failed to create a PanelSlot."), Blueprint, Callsite, Index,
                    Widget->GetName());
                return false;
            }
            return true;
        }

        bool CodexUmgApplySlotProperties(UWidgetBlueprint* Blueprint, UWidget* Widget, const TSharedRef<FJsonObject>& Operation,
            FWriteMutationContext& Context, FUmgOperationError& OutError, int32 Index)
        {
            const TSharedPtr<FJsonObject>* SlotProperties = nullptr;
            if (!Operation->TryGetObjectField(TEXT("slotProperties"), SlotProperties)) return true;
            if (!Widget || !Widget->Slot)
            {
                OutError = CodexUmgError(TEXT("UmgPanelSlotNotFound"), TEXT("'slotProperties' requires attachment to a panel."),
                    Blueprint, TEXT("CodexUmgApplySlotProperties"), Index, Widget ? Widget->GetName() : FString());
                return false;
            }
            FWritePipelineError ModifyError;
            if (!Context.Modify(Widget->Slot, ModifyError))
            {
                OutError = CodexUmgError(ModifyError.Code, ModifyError.Message, Blueprint, ModifyError.UECallsite, Index,
                    Widget->GetName());
                return false;
            }
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*SlotProperties)->Values)
            {
                FBlueprintOperationError PropertyError;
                if (!FBlueprintTypeSystem::SetPropertyValue(Widget->Slot, Pair.Key, Pair.Value, PropertyError, CodexUmgAssetPath(Blueprint), Index))
                {
                    OutError = CodexUmgError(PropertyError.Code, PropertyError.Message, Blueprint,
                        PropertyError.UECallsite, Index, Widget->GetName(), FString(), PropertyError.Details);
                    return false;
                }
            }
            Widget->Slot->PostEditChange();
            return true;
        }

        TSharedRef<FJsonObject> CodexUmgPropertySnapshot(UObject* Object, const UWidgetBlueprint* Blueprint)
        {
            TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
            if (!Object) return Json;
            for (TFieldIterator<FProperty> It(Object->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
            {
                FProperty* Property = *It;
                if (!Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)) continue;
                FBlueprintOperationError PropertyError;
                const void* Address = Property->ContainerPtrToValuePtr<void>(Object);
                TSharedPtr<FJsonValue> Value = FBlueprintTypeSystem::PropertyValueToJson(Property, Address, PropertyError, CodexUmgAssetPath(Blueprint));
                if (Value.IsValid()) Json->SetField(Property->GetName(), Value);
            }
            return Json;
        }

        FString CodexUmgCanonicalJson(const TSharedPtr<FJsonValue>& Value);

        FString CodexUmgCanonicalObject(const TSharedRef<FJsonObject>& Object)
        {
            TArray<FString> Keys;
            Object->Values.GetKeys(Keys);
            Keys.Sort();
            FString Result(TEXT("{"));
            for (int32 Index = 0; Index < Keys.Num(); ++Index)
            {
                if (Index) Result += TEXT(",");
                Result += FString::Printf(TEXT("\"%s\":"), *Keys[Index].ReplaceCharWithEscapedChar());
                Result += CodexUmgCanonicalJson(Object->Values[Keys[Index]]);
            }
            return Result + TEXT("}");
        }

        FString CodexUmgCanonicalJson(const TSharedPtr<FJsonValue>& Value)
        {
            if (!Value.IsValid() || Value->IsNull()) return TEXT("null");
            if (Value->Type == EJson::Object) return CodexUmgCanonicalObject(Value->AsObject().ToSharedRef());
            if (Value->Type == EJson::Array)
            {
                FString Result(TEXT("["));
                const TArray<TSharedPtr<FJsonValue>>& Values = Value->AsArray();
                for (int32 Index = 0; Index < Values.Num(); ++Index)
                {
                    if (Index) Result += TEXT(",");
                    Result += CodexUmgCanonicalJson(Values[Index]);
                }
                return Result + TEXT("]");
            }
            FString Result;
            TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Result);
            FJsonSerializer::Serialize(Value.ToSharedRef(), TEXT(""), Writer);
            Writer->Close();
            return Result;
        }

        FString CodexUmgBytesToHex(const uint8* Bytes, const int32 Count)
        {
            static const TCHAR Hex[] = TEXT("0123456789abcdef");
            FString Result;
            Result.Reserve(Count * 2);
            for (int32 Index = 0; Index < Count; ++Index)
            {
                Result.AppendChar(Hex[(Bytes[Index] >> 4) & 0x0f]);
                Result.AppendChar(Hex[Bytes[Index] & 0x0f]);
            }
            return Result;
        }

        FString CodexUmgSha1(const FString& Value)
        {
            const FTCHARToUTF8 Utf8(*Value);
            uint8 Hash[FSHA1::DigestSize];
            FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Hash);
            return CodexUmgBytesToHex(Hash, FSHA1::DigestSize);
        }

        template<typename ChannelType, typename WriteValue>
        void CodexUmgAppendChannelKeys(FMovieSceneChannel* Channel, TArray<TSharedPtr<FJsonValue>>& OutKeys, WriteValue&& Write)
        {
            ChannelType* Typed = static_cast<ChannelType*>(Channel);
            const auto Data = Typed->GetData();
            const auto Times = Data.GetTimes();
            const auto Values = Data.GetValues();
            for (int32 KeyIndex = 0; KeyIndex < Times.Num(); ++KeyIndex)
            {
                TSharedRef<FJsonObject> Key = MakeShared<FJsonObject>();
                Key->SetNumberField(TEXT("frame"), Times[KeyIndex].Value);
                Write(Key, Values[KeyIndex]);
                OutKeys.Add(MakeShared<FJsonValueObject>(Key));
            }
        }

        TSharedRef<FJsonObject> CodexUmgChannelSnapshot(FMovieSceneChannel* Channel, const FName TypeName)
        {
            TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
            Json->SetStringField(TEXT("type"), TypeName.ToString());
            TArray<TSharedPtr<FJsonValue>> Keys;
            if (TypeName == FMovieSceneFloatChannel::StaticStruct()->GetFName())
                CodexUmgAppendChannelKeys<FMovieSceneFloatChannel>(Channel, Keys,
                    [](const TSharedRef<FJsonObject>& Key, const FMovieSceneFloatValue& Value)
                    { Key->SetNumberField(TEXT("value"), Value.Value); });
            else if (TypeName == FMovieSceneBoolChannel::StaticStruct()->GetFName())
                CodexUmgAppendChannelKeys<FMovieSceneBoolChannel>(Channel, Keys,
                    [](const TSharedRef<FJsonObject>& Key, const bool Value) { Key->SetBoolField(TEXT("value"), Value); });
            else if (TypeName == FMovieSceneByteChannel::StaticStruct()->GetFName())
                CodexUmgAppendChannelKeys<FMovieSceneByteChannel>(Channel, Keys,
                    [](const TSharedRef<FJsonObject>& Key, const uint8 Value) { Key->SetNumberField(TEXT("value"), Value); });
            else if (TypeName == FMovieSceneIntegerChannel::StaticStruct()->GetFName())
                CodexUmgAppendChannelKeys<FMovieSceneIntegerChannel>(Channel, Keys,
                    [](const TSharedRef<FJsonObject>& Key, const int32 Value) { Key->SetNumberField(TEXT("value"), Value); });
            else if (TypeName == FMovieSceneStringChannel::StaticStruct()->GetFName())
                CodexUmgAppendChannelKeys<FMovieSceneStringChannel>(Channel, Keys,
                    [](const TSharedRef<FJsonObject>& Key, const FString& Value) { Key->SetStringField(TEXT("value"), Value); });
            Json->SetArrayField(TEXT("keys"), Keys);
            return Json;
        }

        bool CodexUmgResolveAnimationObjects(UWidgetBlueprint* Blueprint, const TSharedRef<FJsonObject>& Operation,
            UWidgetAnimation*& OutAnimation, UMovieScene*& OutMovieScene, FMovieSceneBinding*& OutBinding,
            UMovieSceneTrack*& OutTrack, UMovieSceneSection*& OutSection, FUmgOperationError& OutError,
            int32 Index, const FString& Callsite, bool bNeedBinding, bool bNeedTrack, bool bNeedSection)
        {
            FString AnimationName;
            if (!CodexUmgRequireString(Operation, TEXT("animation"), AnimationName, OutError, Blueprint, Index, Callsite)) return false;
            OutAnimation = CodexUmgFindAnimation(Blueprint, AnimationName, OutError, Index, Callsite);
            if (!OutAnimation) return false;
            OutMovieScene = OutAnimation->MovieScene;
            if (!OutMovieScene)
            {
                OutError = CodexUmgError(TEXT("UmgAnimationInvalid"), TEXT("Animation has no MovieScene."), Blueprint, Callsite, Index,
                    FString(), AnimationName);
                return false;
            }
            OutBinding = nullptr;
            OutTrack = nullptr;
            OutSection = nullptr;
            if (!bNeedBinding) return true;
            FString GuidText;
            FGuid Guid;
            if (!CodexUmgRequireString(Operation, TEXT("bindingGuid"), GuidText, OutError, Blueprint, Index, Callsite)
                || !CodexUmgParseGuid(GuidText, Guid))
            {
                if (!OutError.IsSet()) OutError = CodexUmgError(TEXT("UmgInvalidBindingGuid"), TEXT("'bindingGuid' is not a valid GUID."),
                    Blueprint, Callsite, Index, FString(), AnimationName);
                return false;
            }
            OutBinding = OutMovieScene->FindBinding(Guid);
            if (!OutBinding)
            {
                OutError = CodexUmgError(TEXT("UmgAnimationBindingNotFound"), TEXT("MovieScene binding was not found."),
                    Blueprint, Callsite, Index, FString(), AnimationName, TArray<FString>{GuidText});
                return false;
            }
            if (!bNeedTrack) return true;
            int32 TrackIndex = INDEX_NONE;
            if (!CodexUmgRequireInteger(Operation, TEXT("trackIndex"), TrackIndex, OutError, Blueprint, Index, Callsite)) return false;
            const TArray<UMovieSceneTrack*>& Tracks = OutBinding->GetTracks();
            if (!Tracks.IsValidIndex(TrackIndex) || !Tracks[TrackIndex])
            {
                OutError = CodexUmgError(TEXT("UmgAnimationTrackNotFound"), TEXT("'trackIndex' does not identify a track."),
                    Blueprint, Callsite, Index, FString(), AnimationName);
                return false;
            }
            OutTrack = Tracks[TrackIndex];
            if (!bNeedSection) return true;
            int32 SectionIndex = INDEX_NONE;
            if (!CodexUmgRequireInteger(Operation, TEXT("sectionIndex"), SectionIndex, OutError, Blueprint, Index, Callsite)) return false;
            const TArray<UMovieSceneSection*>& Sections = OutTrack->GetAllSections();
            if (!Sections.IsValidIndex(SectionIndex) || !Sections[SectionIndex])
            {
                OutError = CodexUmgError(TEXT("UmgAnimationSectionNotFound"), TEXT("'sectionIndex' does not identify a section."),
                    Blueprint, Callsite, Index, FString(), AnimationName);
                return false;
            }
            OutSection = Sections[SectionIndex];
            return true;
        }

        template<typename ChannelType, typename ValueType>
        bool CodexUmgMutateTypedChannel(ChannelType* Channel, const FString& Action, const int32 Frame, const int32 NewFrame,
            const ValueType& Value, FUmgOperationError& OutError, UWidgetBlueprint* Blueprint, int32 Index,
            const FString& AnimationName)
        {
            auto Data = Channel->GetData();
            int32 KeyIndex = Data.FindKey(FFrameNumber(Frame));
            if (Action == TEXT("add"))
            {
                if (KeyIndex != INDEX_NONE)
                {
                    OutError = CodexUmgError(TEXT("UmgAnimationKeyAlreadyExists"), TEXT("A key already exists at the requested frame."),
                        Blueprint, TEXT("CodexUmgMutateTypedChannel"), Index, FString(), AnimationName);
                    return false;
                }
                Data.AddKey(FFrameNumber(Frame), Value);
                return true;
            }
            if (KeyIndex == INDEX_NONE)
            {
                OutError = CodexUmgError(TEXT("UmgAnimationKeyNotFound"), TEXT("No key exists at the requested frame."),
                    Blueprint, TEXT("CodexUmgMutateTypedChannel"), Index, FString(), AnimationName);
                return false;
            }
            if (Action == TEXT("remove"))
            {
                Data.RemoveKey(KeyIndex);
                return true;
            }
            if (NewFrame != Frame && Data.FindKey(FFrameNumber(NewFrame)) != INDEX_NONE)
            {
                OutError = CodexUmgError(TEXT("UmgAnimationKeyAlreadyExists"), TEXT("A key already exists at 'newFrame'."),
                    Blueprint, TEXT("CodexUmgMutateTypedChannel"), Index, FString(), AnimationName);
                return false;
            }
            Data.RemoveKey(KeyIndex);
            Data.AddKey(FFrameNumber(NewFrame), Value);
            return true;
        }

        class FCodexUmgWriteOperation final : public IWriteOperation
        {
        public:
            FCodexUmgWriteOperation(UWidgetBlueprint* InBlueprint, const TSharedRef<FJsonObject>& InOperation, int32 InIndex,
                TArray<FUmgOperationResult>* InResults)
                : Blueprint(InBlueprint), Operation(InOperation), Index(InIndex), Results(InResults) {}

            virtual int32 GetOperationIndex() const override { return Index; }

            virtual void GatherPreflight(FPreflightRequest& InOutRequest) const override
            {
                if (!Blueprint.IsValid() || !Blueprint->GetOutermost()) return;
                const FString PackageName = Blueprint->GetOutermost()->GetName();
                InOutRequest.TargetPackageNames.AddUnique(PackageName);
                InOutRequest.CompilePackageNames.AddUnique(PackageName);
                FString OperationName;
                FString ClassPath;
                Operation->TryGetStringField(TEXT("operation"), OperationName);
                if ((OperationName == TEXT("widget.add") || OperationName == TEXT("animation.track.add"))
                    && Operation->TryGetStringField(TEXT("classPath"), ClassPath))
                {
                    FTypeReferenceRequirement Requirement;
                    Requirement.ObjectPath = ClassPath;
                    Requirement.ExpectedClassPath = UClass::StaticClass()->GetPathName();
                    Requirement.OperationIndex = Index;
                    InOutRequest.TypeReferences.Add(Requirement);
                }
            }

            virtual bool Apply(FWriteMutationContext& Context, FWritePipelineError& OutError) override
            {
                FUmgOperationResult Result;
                FUmgOperationError ErrorValue;
                if (!FBlueprintUmgOperations::Apply(Blueprint.Get(), Operation, Context, Result, ErrorValue, Index))
                {
                    OutError.Code = ErrorValue.Code;
                    OutError.Message = ErrorValue.Message;
                    OutError.AssetPath = ErrorValue.AssetPath;
                    OutError.UECallsite = ErrorValue.UECallsite;
                    OutError.OperationIndex = Index;
                    return false;
                }
                if (Results) Results->Add(Result);
                return true;
            }

            virtual bool VerifyInMemory(FWritePipelineError& OutError) const override
            {
                TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>();
                FUmgOperationError ErrorValue;
                if (!FBlueprintUmgOperations::Inspect(Blueprint.Get(), Snapshot, ErrorValue))
                {
                    OutError.Code = ErrorValue.Code;
                    OutError.Message = ErrorValue.Message;
                    OutError.AssetPath = ErrorValue.AssetPath;
                    OutError.UECallsite = ErrorValue.UECallsite;
                    OutError.OperationIndex = Index;
                    return false;
                }
                return true;
            }

        private:
            TWeakObjectPtr<UWidgetBlueprint> Blueprint;
            TSharedRef<FJsonObject> Operation;
            int32 Index;
            TArray<FUmgOperationResult>* Results;
        };
    }

    TSharedRef<FJsonObject> FUmgOperationError::ToJson() const
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("code"), Code);
        Json->SetStringField(TEXT("message"), Message);
        Json->SetStringField(TEXT("ueCallsite"), UECallsite);
        if (!AssetPath.IsEmpty()) Json->SetStringField(TEXT("assetPath"), AssetPath);
        if (!WidgetPath.IsEmpty()) Json->SetStringField(TEXT("widgetPath"), WidgetPath);
        if (!AnimationName.IsEmpty()) Json->SetStringField(TEXT("animationName"), AnimationName);
        if (OperationIndex != INDEX_NONE) Json->SetNumberField(TEXT("operationIndex"), OperationIndex);
        TArray<TSharedPtr<FJsonValue>> JsonDetails;
        for (const FString& Detail : Details) JsonDetails.Add(MakeShared<FJsonValueString>(Detail));
        if (JsonDetails.Num()) Json->SetArrayField(TEXT("details"), JsonDetails);
        return Json;
    }

    TSharedRef<FJsonObject> FUmgOperationResult::ToJson() const
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetBoolField(TEXT("changed"), bChanged);
        Json->SetStringField(TEXT("assetPath"), AssetPath);
        TArray<TSharedPtr<FJsonValue>> Packages;
        for (const FString& Package : ImpactPackages) Packages.Add(MakeShared<FJsonValueString>(Package));
        Json->SetArrayField(TEXT("impactPackages"), Packages);
        Json->SetObjectField(TEXT("data"), Data.IsValid() ? Data.ToSharedRef() : MakeShared<FJsonObject>());
        return Json;
    }

    bool FBlueprintUmgOperations::Apply(UWidgetBlueprint* Blueprint, const TSharedRef<FJsonObject>& Operation,
        FWriteMutationContext& Context, FUmgOperationResult& OutResult, FUmgOperationError& OutError, const int32 OperationIndex)
    {
        OutResult = FUmgOperationResult();
        OutError = FUmgOperationError();
        const FString Callsite(TEXT("FBlueprintUmgOperations::Apply"));
        if (!IsInGameThread())
        {
            OutError = CodexUmgError(TEXT("UmgWrongThread"), TEXT("UMG mutations must run on the game thread."), Blueprint, Callsite, OperationIndex);
            return false;
        }
        if (!Blueprint || !Blueprint->WidgetTree)
        {
            OutError = CodexUmgError(TEXT("UmgInvalidBlueprint"), TEXT("A loaded UWidgetBlueprint with a WidgetTree is required."),
                Blueprint, Callsite, OperationIndex);
            return false;
        }
        FString Type;
        FString OperationAssetPath;
        if (!CodexUmgRequireString(Operation, TEXT("operation"), Type, OutError, Blueprint, OperationIndex, Callsite)
            || !CodexUmgRequireString(Operation, TEXT("assetPath"), OperationAssetPath, OutError, Blueprint, OperationIndex, Callsite)) return false;
        if (OperationAssetPath != Blueprint->GetPathName())
        {
            OutError = CodexUmgError(TEXT("UmgAssetPathMismatch"), TEXT("'assetPath' does not identify the supplied Widget Blueprint."),
                Blueprint, Callsite, OperationIndex, OperationAssetPath);
            return false;
        }
        FWritePipelineError ModifyError;
        if (!CodexUmgModifyObject(Context, Blueprint, ModifyError, OutError, Blueprint, OperationIndex)
            || !CodexUmgModifyObject(Context, Blueprint->WidgetTree, ModifyError, OutError, Blueprint, OperationIndex)) return false;

        OutResult.AssetPath = CodexUmgAssetPath(Blueprint);
        OutResult.ImpactPackages.Add(Blueprint->GetOutermost()->GetName());
        OutResult.Data = MakeShared<FJsonObject>();
        bool bStructural = false;

        if (Type == TEXT("widget.add"))
        {
            FString Name, ClassPath;
            if (!CodexUmgRequireString(Operation, TEXT("name"), Name, OutError, Blueprint, OperationIndex, Callsite)
                || !CodexUmgRequireString(Operation, TEXT("classPath"), ClassPath, OutError, Blueprint, OperationIndex, Callsite)) return false;
            if (Blueprint->WidgetTree->FindWidget(FName(*Name)))
            {
                OutError = CodexUmgError(TEXT("UmgWidgetAlreadyExists"), FString::Printf(TEXT("Widget '%s' already exists."), *Name),
                    Blueprint, Callsite, OperationIndex, Name);
                return false;
            }
            UClass* Class = LoadObject<UClass>(nullptr, *ClassPath);
            if (!Class || !Class->IsChildOf(UWidget::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract))
            {
                OutError = CodexUmgError(TEXT("UmgInvalidWidgetClass"), FString::Printf(TEXT("'%s' is not a concrete UWidget class."), *ClassPath),
                    Blueprint, TEXT("LoadObject<UClass>"), OperationIndex, Name);
                return false;
            }
            UWidget* Widget = Blueprint->WidgetTree->ConstructWidget<UWidget>(Class, FName(*Name));
            if (!Widget)
            {
                OutError = CodexUmgError(TEXT("UmgWidgetConstructionFailed"), TEXT("UWidgetTree::ConstructWidget returned null."),
                    Blueprint, TEXT("UWidgetTree::ConstructWidget"), OperationIndex, Name);
                return false;
            }
            if (!Context.Modify(Widget, ModifyError))
            {
                OutError = CodexUmgError(ModifyError.Code, ModifyError.Message, Blueprint, ModifyError.UECallsite, OperationIndex, Name);
                return false;
            }
            FString ParentName;
            if (Operation->TryGetStringField(TEXT("parent"), ParentName) && !ParentName.IsEmpty())
            {
                UWidget* Parent = Blueprint->WidgetTree->FindWidget(FName(*ParentName));
                if (Parent && !CodexUmgModifyObject(Context, Parent, ModifyError, OutError, Blueprint, OperationIndex, ParentName)) return false;
            }
            if (!CodexUmgAttachWidget(Blueprint, Widget, Operation, OutError, OperationIndex, Callsite)) return false;
            if (!CodexUmgApplySlotProperties(Blueprint, Widget, Operation, Context, OutError, OperationIndex)) return false;
            const TSharedPtr<FJsonObject>* Properties = nullptr;
            if (Operation->TryGetObjectField(TEXT("properties"), Properties))
            {
                for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Properties)->Values)
                {
                    FBlueprintOperationError PropertyError;
                    if (!FBlueprintTypeSystem::SetPropertyValue(Widget, Pair.Key, Pair.Value, PropertyError, CodexUmgAssetPath(Blueprint), OperationIndex))
                    {
                        OutError = CodexUmgError(PropertyError.Code, PropertyError.Message, Blueprint, PropertyError.UECallsite,
                            OperationIndex, Name, FString(), PropertyError.Details);
                        return false;
                    }
                }
            }
            Widget->PostEditChange();
            OutResult.Data->SetStringField(TEXT("widget"), Widget->GetName());
            bStructural = true;
        }
        else if (Type == TEXT("widget.remove"))
        {
            FString Name;
            if (!CodexUmgRequireString(Operation, TEXT("widget"), Name, OutError, Blueprint, OperationIndex, Callsite)) return false;
            UWidget* Widget = CodexUmgFindWidget(Blueprint, Name, OutError, OperationIndex, Callsite);
            if (!Widget) return false;
            TArray<UWidget*> Removed;
            UWidgetTree::ForWidgetAndChildren(Widget, [&](UWidget* Item) { if (Item) Removed.AddUnique(Item); });
            for (UWidget* Item : Removed)
                if (!CodexUmgModifyObject(Context, Item, ModifyError, OutError, Blueprint, OperationIndex, Item->GetName())) return false;
            int32 ExistingChildIndex = INDEX_NONE;
            if (UPanelWidget* ExistingParent = UWidgetTree::FindWidgetParent(Widget, ExistingChildIndex))
            {
                if (!CodexUmgModifyObject(Context, ExistingParent, ModifyError, OutError, Blueprint, OperationIndex, Name)) return false;
            }
            else
            {
                UWidget* ExistingOwner = nullptr; FName ExistingSlot;
                if (CodexUmgFindNamedSlotOwner(Blueprint, Widget, ExistingOwner, ExistingSlot)
                    && !CodexUmgModifyObject(Context, ExistingOwner, ModifyError, OutError, Blueprint, OperationIndex, Name)) return false;
            }
            TSet<FName> Names;
            for (UWidget* Item : Removed) Names.Add(Item->GetFName());
            Blueprint->Bindings.RemoveAll([&](const FDelegateEditorBinding& Binding) { return Names.Contains(FName(*Binding.ObjectName)); });
            TArray<UK2Node_ComponentBoundEvent*> BoundEvents;
            FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, BoundEvents);
            for (UK2Node_ComponentBoundEvent* BoundEvent : BoundEvents)
            {
                if (!BoundEvent || !Names.Contains(BoundEvent->ComponentPropertyName)) continue;
                if (!CodexUmgModifyObject(Context, BoundEvent, ModifyError, OutError, Blueprint, OperationIndex, Name)
                    || !CodexUmgModifyObject(Context, BoundEvent->GetGraph(), ModifyError, OutError, Blueprint, OperationIndex, Name)) return false;
                FBlueprintEditorUtils::RemoveNode(Blueprint, BoundEvent, true);
            }
            for (UWidgetAnimation* Animation : Blueprint->Animations)
            {
                if (!Animation) continue;
                if (!CodexUmgModifyObject(Context, Animation, ModifyError, OutError, Blueprint, OperationIndex,
                    Name, Animation->GetDisplayLabel())) return false;
                if (Animation->MovieScene && !CodexUmgModifyObject(Context, Animation->MovieScene, ModifyError, OutError,
                    Blueprint, OperationIndex, Name, Animation->GetDisplayLabel())) return false;
                TArray<FGuid> RemovedGuids;
                Animation->AnimationBindings.RemoveAll([&](const FWidgetAnimationBinding& Binding)
                {
                    if (Names.Contains(Binding.WidgetName)) { RemovedGuids.Add(Binding.AnimationGuid); return true; }
                    return false;
                });
                for (const FGuid& Guid : RemovedGuids) Animation->MovieScene->RemovePossessable(Guid);
            }
            bool bRemoved = false;
            UWidget* NamedSlotOwner = nullptr; FName NamedSlotName;
            if (CodexUmgFindNamedSlotOwner(Blueprint, Widget, NamedSlotOwner, NamedSlotName))
            {
                Cast<INamedSlotInterface>(NamedSlotOwner)->SetContentForSlot(NamedSlotName, nullptr);
                bRemoved = true;
            }
            else
            {
                bRemoved = Blueprint->WidgetTree->RemoveWidget(Widget);
            }
            if (!bRemoved)
            {
                OutError = CodexUmgError(TEXT("UmgWidgetRemoveFailed"), TEXT("UE could not remove the widget hierarchy."),
                    Blueprint, TEXT("UWidgetTree::RemoveWidget"), OperationIndex, Name);
                return false;
            }
            bStructural = true;
        }
        else if (Type == TEXT("widget.rename"))
        {
            FString Name, NewName;
            if (!CodexUmgRequireString(Operation, TEXT("widget"), Name, OutError, Blueprint, OperationIndex, Callsite)
                || !CodexUmgRequireString(Operation, TEXT("newName"), NewName, OutError, Blueprint, OperationIndex, Callsite)) return false;
            UWidget* Widget = CodexUmgFindWidget(Blueprint, Name, OutError, OperationIndex, Callsite);
            if (!Widget) return false;
            const FName NewFName(*NewName);
            if (Blueprint->WidgetTree->FindWidget(NewFName) || FindObject<UObject>(Blueprint->WidgetTree, *NewName))
            {
                OutError = CodexUmgError(TEXT("UmgWidgetAlreadyExists"), FString::Printf(TEXT("Widget '%s' already exists."), *NewName),
                    Blueprint, Callsite, OperationIndex, Name);
                return false;
            }
            if (!CodexUmgModifyObject(Context, Widget, ModifyError, OutError, Blueprint, OperationIndex, Name)) return false;
            FBlueprintEditorUtils::ReplaceVariableReferences(Blueprint, Widget->GetFName(), NewFName);
            for (FDelegateEditorBinding& Binding : Blueprint->Bindings) if (Binding.ObjectName == Name) Binding.ObjectName = NewName;
            for (UWidgetAnimation* Animation : Blueprint->Animations)
            {
                if (!Animation) continue;
                if (!CodexUmgModifyObject(Context, Animation, ModifyError, OutError, Blueprint, OperationIndex,
                    Name, Animation->GetDisplayLabel())) return false;
                for (FWidgetAnimationBinding& Binding : Animation->AnimationBindings)
                {
                    if (Binding.WidgetName != Widget->GetFName()) continue;
                    Binding.WidgetName = NewFName;
                    if (Animation->MovieScene)
                    {
                        if (!CodexUmgModifyObject(Context, Animation->MovieScene, ModifyError, OutError, Blueprint,
                            OperationIndex, Name, Animation->GetDisplayLabel())) return false;
                        if (Binding.SlotWidgetName == NAME_None)
                            if (FMovieScenePossessable* Possessable = Animation->MovieScene->FindPossessable(Binding.AnimationGuid))
                                Possessable->SetName(NewName);
                    }
                }
            }
            TArray<UWidget*> NavigationWidgets;
            Blueprint->WidgetTree->GetAllWidgets(NavigationWidgets);
            for (UWidget* NavigationWidget : NavigationWidgets)
            {
                if (!NavigationWidget || !NavigationWidget->Navigation) continue;
                if (!CodexUmgModifyObject(Context, NavigationWidget->Navigation, ModifyError, OutError, Blueprint,
                    OperationIndex, NavigationWidget->GetName())) return false;
                NavigationWidget->Navigation->TryToRenameBinding(Widget->GetFName(), NewFName);
            }
            Widget->SetDisplayLabel(NewName);
            if (!Widget->Rename(*NewName, Blueprint->WidgetTree, REN_DontCreateRedirectors | REN_ForceNoResetLoaders))
            {
                OutError = CodexUmgError(TEXT("UmgWidgetRenameFailed"), TEXT("UObject::Rename rejected the new widget name."),
                    Blueprint, TEXT("UObject::Rename"), OperationIndex, Name);
                return false;
            }
            OutResult.Data->SetStringField(TEXT("widget"), NewName);
            bStructural = true;
        }
        else if (Type == TEXT("widget.reparent") || Type == TEXT("namedSlot.set"))
        {
            FString Name;
            if (!CodexUmgRequireString(Operation, TEXT("widget"), Name, OutError, Blueprint, OperationIndex, Callsite)) return false;
            UWidget* Widget = CodexUmgFindWidget(Blueprint, Name, OutError, OperationIndex, Callsite);
            if (!Widget || !CodexUmgModifyObject(Context, Widget, ModifyError, OutError, Blueprint, OperationIndex, Name)) return false;
            if (Widget->Slot && !CodexUmgModifyObject(Context, Widget->Slot, ModifyError, OutError, Blueprint, OperationIndex, Name)) return false;
            int32 ExistingChildIndex = INDEX_NONE;
            if (UPanelWidget* ExistingParent = UWidgetTree::FindWidgetParent(Widget, ExistingChildIndex))
            {
                if (!CodexUmgModifyObject(Context, ExistingParent, ModifyError, OutError, Blueprint, OperationIndex, Name)) return false;
            }
            else
            {
                UWidget* ExistingOwner = nullptr; FName ExistingSlot;
                if (CodexUmgFindNamedSlotOwner(Blueprint, Widget, ExistingOwner, ExistingSlot)
                    && !CodexUmgModifyObject(Context, ExistingOwner, ModifyError, OutError, Blueprint, OperationIndex, Name)) return false;
            }
            FString ParentName;
            if (Operation->TryGetStringField(TEXT("parent"), ParentName) && !ParentName.IsEmpty())
            {
                UWidget* NewParent = Blueprint->WidgetTree->FindWidget(FName(*ParentName));
                if (NewParent && !CodexUmgModifyObject(Context, NewParent, ModifyError, OutError, Blueprint,
                    OperationIndex, ParentName)) return false;
            }
            if (!CodexUmgDetachWidget(Blueprint, Widget) || !CodexUmgAttachWidget(Blueprint, Widget, Operation, OutError, OperationIndex, Callsite)) return false;
            if (!CodexUmgApplySlotProperties(Blueprint, Widget, Operation, Context, OutError, OperationIndex)) return false;
            bStructural = true;
        }
        else if (Type == TEXT("namedSlot.clear"))
        {
            FString ParentName, SlotName;
            if (!CodexUmgRequireString(Operation, TEXT("parent"), ParentName, OutError, Blueprint, OperationIndex, Callsite)
                || !CodexUmgRequireString(Operation, TEXT("namedSlot"), SlotName, OutError, Blueprint, OperationIndex, Callsite)) return false;
            UWidget* Parent = CodexUmgFindWidget(Blueprint, ParentName, OutError, OperationIndex, Callsite);
            if (!Parent || !Parent->GetClass()->ImplementsInterface(UNamedSlotInterface::StaticClass()))
            {
                if (!OutError.IsSet()) OutError = CodexUmgError(TEXT("UmgNamedSlotUnsupported"), TEXT("Parent has no named slots."),
                    Blueprint, Callsite, OperationIndex, ParentName);
                return false;
            }
            INamedSlotInterface* Interface = Cast<INamedSlotInterface>(Parent);
            TArray<FName> Slots;
            Interface->GetSlotNames(Slots);
            const FName RequestedSlot(*SlotName);
            if (!Slots.Contains(RequestedSlot))
            {
                TArray<FString> Candidates;
                for (const FName Slot : Slots) Candidates.Add(Slot.ToString());
                OutError = CodexUmgError(TEXT("UmgNamedSlotNotFound"), TEXT("The requested named slot does not exist."),
                    Blueprint, Callsite, OperationIndex, ParentName, FString(), Candidates);
                return false;
            }
            UWidget* Existing = Interface->GetContentForSlot(RequestedSlot);
            if (!Existing)
            {
                OutError = CodexUmgError(TEXT("UmgNamedSlotEmpty"), TEXT("The named slot is already empty."), Blueprint, Callsite,
                    OperationIndex, ParentName);
                return false;
            }
            if (!CodexUmgModifyObject(Context, Parent, ModifyError, OutError, Blueprint, OperationIndex, ParentName)
                || !CodexUmgModifyObject(Context, Existing, ModifyError, OutError, Blueprint, OperationIndex, Existing->GetName())) return false;
            Interface->SetContentForSlot(RequestedSlot, nullptr);
            bStructural = true;
        }
        else if (Type == TEXT("slot.property.set") || Type == TEXT("widget.property.set"))
        {
            FString Name, Property;
            TSharedPtr<FJsonValue> Value;
            if (!CodexUmgRequireString(Operation, TEXT("widget"), Name, OutError, Blueprint, OperationIndex, Callsite)
                || !CodexUmgRequireString(Operation, TEXT("property"), Property, OutError, Blueprint, OperationIndex, Callsite)
                || !CodexUmgRequireValue(Operation, TEXT("value"), Value, OutError, Blueprint, OperationIndex, Callsite)) return false;
            UWidget* Widget = CodexUmgFindWidget(Blueprint, Name, OutError, OperationIndex, Callsite);
            UObject* Target = Type == TEXT("slot.property.set") ? static_cast<UObject*>(Widget ? Widget->Slot : nullptr) : Widget;
            if (!Target)
            {
                if (!OutError.IsSet()) OutError = CodexUmgError(TEXT("UmgPanelSlotNotFound"), TEXT("Widget has no PanelSlot."),
                    Blueprint, Callsite, OperationIndex, Name);
                return false;
            }
            if (!CodexUmgModifyObject(Context, Target, ModifyError, OutError, Blueprint, OperationIndex, Name)) return false;
            FBlueprintOperationError PropertyError;
            if (!FBlueprintTypeSystem::SetPropertyValue(Target, Property, Value, PropertyError, CodexUmgAssetPath(Blueprint), OperationIndex))
            {
                OutError = CodexUmgError(PropertyError.Code, PropertyError.Message, Blueprint, PropertyError.UECallsite,
                    OperationIndex, Name, FString(), PropertyError.Details);
                return false;
            }
            Target->PostEditChange();
        }
        else if (Type == TEXT("widget.variable.set"))
        {
            FString Name;
            bool bVariable = false;
            if (!CodexUmgRequireString(Operation, TEXT("widget"), Name, OutError, Blueprint, OperationIndex, Callsite)
                || !Operation->TryGetBoolField(TEXT("isVariable"), bVariable))
            {
                if (!OutError.IsSet()) OutError = CodexUmgError(TEXT("UmgInvalidArgument"), TEXT("'isVariable' must be boolean."),
                    Blueprint, Callsite, OperationIndex, Name);
                return false;
            }
            UWidget* Widget = CodexUmgFindWidget(Blueprint, Name, OutError, OperationIndex, Callsite);
            if (!Widget || !CodexUmgModifyObject(Context, Widget, ModifyError, OutError, Blueprint, OperationIndex, Name)) return false;
            Widget->bIsVariable = bVariable;
            bStructural = true;
        }
        else if (Type == TEXT("event.bind") || Type == TEXT("event.unbind"))
        {
            FString WidgetName, EventName;
            if (!CodexUmgRequireString(Operation, TEXT("widget"), WidgetName, OutError, Blueprint, OperationIndex, Callsite)
                || !CodexUmgRequireString(Operation, TEXT("event"), EventName, OutError, Blueprint, OperationIndex, Callsite)) return false;
            UWidget* Widget = CodexUmgFindWidget(Blueprint, WidgetName, OutError, OperationIndex, Callsite);
            if (!Widget) return false;
            FMulticastDelegateProperty* Delegate = FindFProperty<FMulticastDelegateProperty>(Widget->GetClass(), FName(*EventName));
            if (!Delegate)
            {
                OutError = CodexUmgError(TEXT("UmgEventNotFound"), TEXT("The widget class has no multicast delegate with this name."),
                    Blueprint, Callsite, OperationIndex, WidgetName, FString(), TArray<FString>{EventName});
                return false;
            }
            const UK2Node_ComponentBoundEvent* Existing = FKismetEditorUtilities::FindBoundEventForComponent(
                Blueprint, Delegate->GetFName(), Widget->GetFName());
            if (Type == TEXT("event.unbind"))
            {
                if (!Existing)
                {
                    OutError = CodexUmgError(TEXT("UmgEventBindingNotFound"), TEXT("The requested widget event binding does not exist."),
                        Blueprint, Callsite, OperationIndex, WidgetName, FString(), TArray<FString>{EventName});
                    return false;
                }
                UK2Node_ComponentBoundEvent* MutableNode = const_cast<UK2Node_ComponentBoundEvent*>(Existing);
                if (!CodexUmgModifyObject(Context, MutableNode, ModifyError, OutError, Blueprint, OperationIndex, WidgetName)
                    || !CodexUmgModifyObject(Context, MutableNode->GetGraph(), ModifyError, OutError, Blueprint, OperationIndex, WidgetName)) return false;
                MutableNode->DestroyNode();
            }
            else
            {
                if (Existing)
                {
                    OutError = CodexUmgError(TEXT("UmgEventBindingAlreadyExists"), TEXT("The widget event is already bound."),
                        Blueprint, Callsite, OperationIndex, WidgetName, FString(), TArray<FString>{EventName});
                    return false;
                }
                FObjectProperty* WidgetProperty = Blueprint->SkeletonGeneratedClass
                    ? FindFProperty<FObjectProperty>(Blueprint->SkeletonGeneratedClass, Widget->GetFName()) : nullptr;
                if (!WidgetProperty)
                {
                    OutError = CodexUmgError(TEXT("UmgEventWidgetVariableUnavailable"),
                        TEXT("Event binding requires a compiled widget variable; expose the widget and compile it before binding the event."),
                        Blueprint, Callsite, OperationIndex, WidgetName, FString(), TArray<FString>{EventName});
                    return false;
                }
                UEdGraph* TargetGraph = Blueprint->GetLastEditedUberGraph();
                if (!TargetGraph)
                {
                    OutError = CodexUmgError(TEXT("UmgEventGraphNotFound"), TEXT("Widget Blueprint has no event graph."),
                        Blueprint, Callsite, OperationIndex, WidgetName, FString(), TArray<FString>{EventName});
                    return false;
                }
                if (!CodexUmgModifyObject(Context, TargetGraph, ModifyError, OutError, Blueprint, OperationIndex, WidgetName)) return false;
                double NodeX = TargetGraph->GetGoodPlaceForNewNode().X;
                double NodeY = TargetGraph->GetGoodPlaceForNewNode().Y;
                Operation->TryGetNumberField(TEXT("x"), NodeX);
                Operation->TryGetNumberField(TEXT("y"), NodeY);
                UK2Node_ComponentBoundEvent* Created = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_ComponentBoundEvent>(
                    TargetGraph, FVector2D(NodeX, NodeY), EK2NewNodeFlags::None,
                    [WidgetProperty, Delegate](UK2Node_ComponentBoundEvent* Node)
                    {
                        Node->InitializeComponentBoundEventParams(WidgetProperty, Delegate);
                    });
                if (!Created)
                {
                    OutError = CodexUmgError(TEXT("UmgEventBindingCreateFailed"), TEXT("UE did not create the component-bound event node."),
                        Blueprint, TEXT("FEdGraphSchemaAction_K2NewNode::SpawnNode"), OperationIndex,
                        WidgetName, FString(), TArray<FString>{EventName});
                    return false;
                }
                if (!CodexUmgModifyObject(Context, Created, ModifyError, OutError, Blueprint, OperationIndex, WidgetName)) return false;
                OutResult.Data->SetStringField(TEXT("nodeGuid"), Created->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
            }
            bStructural = true;
        }
        else if (Type == TEXT("binding.set"))
        {
            FString WidgetName, PropertyName, Kind, Source;
            if (!CodexUmgRequireString(Operation, TEXT("widget"), WidgetName, OutError, Blueprint, OperationIndex, Callsite)
                || !CodexUmgRequireString(Operation, TEXT("property"), PropertyName, OutError, Blueprint, OperationIndex, Callsite)
                || !CodexUmgRequireString(Operation, TEXT("kind"), Kind, OutError, Blueprint, OperationIndex, Callsite)
                || !CodexUmgRequireString(Operation, TEXT("source"), Source, OutError, Blueprint, OperationIndex, Callsite)) return false;
            UWidget* Widget = CodexUmgFindWidget(Blueprint, WidgetName, OutError, OperationIndex, Callsite);
            if (!Widget) return false;
            FDelegateProperty* Delegate = FindFProperty<FDelegateProperty>(Widget->GetClass(), FName(*(PropertyName + TEXT("Delegate"))));
            const bool bAttributeBinding = Delegate != nullptr;
            if (!Delegate) Delegate = FindFProperty<FDelegateProperty>(Widget->GetClass(), FName(*PropertyName));
            if (!Delegate)
            {
                OutError = CodexUmgError(TEXT("UmgBindingTargetNotDelegate"), TEXT("Binding target is not a delegate property."),
                    Blueprint, Callsite, OperationIndex, WidgetName, FString(), TArray<FString>{PropertyName});
                return false;
            }
            FDelegateEditorBinding Binding;
            Binding.ObjectName = WidgetName;
            Binding.PropertyName = FName(*PropertyName);
            if (Kind == TEXT("function"))
            {
                UFunction* Function = Blueprint->SkeletonGeneratedClass
                    ? Blueprint->SkeletonGeneratedClass->FindFunctionByName(FName(*Source), EIncludeSuperFlag::IncludeSuper) : nullptr;
                if (!Function || !Function->IsSignatureCompatibleWith(Delegate->SignatureFunction,
                    UFunction::GetDefaultIgnoredSignatureCompatibilityFlags() | CPF_ReturnParm))
                {
                    OutError = CodexUmgError(TEXT("UmgBindingSignatureMismatch"), TEXT("Source function does not match the delegate signature."),
                        Blueprint, Callsite, OperationIndex, WidgetName, FString(), TArray<FString>{Source});
                    return false;
                }
                if (bAttributeBinding && !Function->HasAnyFunctionFlags(FUNC_Const | FUNC_BlueprintPure))
                {
                    OutError = CodexUmgError(TEXT("UmgBindingFunctionNotPure"), TEXT("Property bindings require a pure or const source function."),
                        Blueprint, Callsite, OperationIndex, WidgetName, FString(), TArray<FString>{Source});
                    return false;
                }
                Binding.FunctionName = Function->GetFName();
                Binding.Kind = EBindingKind::Function;
                UBlueprint::GetGuidFromClassByFieldName<UFunction>(Function->GetOwnerClass(), Function->GetFName(), Binding.MemberGuid);
            }
            else if (Kind == TEXT("property"))
            {
                FProperty* SourceProperty = Blueprint->SkeletonGeneratedClass
                    ? FindFProperty<FProperty>(Blueprint->SkeletonGeneratedClass, FName(*Source)) : nullptr;
                if (!SourceProperty)
                {
                    OutError = CodexUmgError(TEXT("UmgBindingSourceNotFound"), TEXT("Source property was not found on the Widget Blueprint class."),
                        Blueprint, Callsite, OperationIndex, WidgetName, FString(), TArray<FString>{Source});
                    return false;
                }
                FProperty* ReturnProperty = nullptr;
                for (TFieldIterator<FProperty> It(Delegate->SignatureFunction); It; ++It)
                {
                    if (It->HasAnyPropertyFlags(CPF_ReturnParm)) { ReturnProperty = *It; break; }
                }
                if (!ReturnProperty || !ReturnProperty->SameType(SourceProperty))
                {
                    OutError = CodexUmgError(TEXT("UmgBindingPropertyTypeMismatch"),
                        TEXT("Source property type does not match the binding delegate return type."),
                        Blueprint, Callsite, OperationIndex, WidgetName, FString(), TArray<FString>{Source});
                    return false;
                }
                Binding.SourceProperty = SourceProperty->GetFName();
                Binding.SourcePath.Segments.Add(FEditorPropertyPathSegment(SourceProperty));
                Binding.Kind = EBindingKind::Property;
                UBlueprint::GetGuidFromClassByFieldName<FProperty>(Blueprint->SkeletonGeneratedClass,
                    SourceProperty->GetFName(), Binding.MemberGuid);
            }
            else
            {
                OutError = CodexUmgError(TEXT("UmgInvalidBindingKind"), TEXT("'kind' must be 'function' or 'property'."),
                    Blueprint, Callsite, OperationIndex, WidgetName);
                return false;
            }
            Blueprint->Bindings.Remove(Binding);
            Blueprint->Bindings.Add(Binding);
            bStructural = true;
        }
        else if (Type == TEXT("binding.remove"))
        {
            FString WidgetName, PropertyName;
            if (!CodexUmgRequireString(Operation, TEXT("widget"), WidgetName, OutError, Blueprint, OperationIndex, Callsite)
                || !CodexUmgRequireString(Operation, TEXT("property"), PropertyName, OutError, Blueprint, OperationIndex, Callsite)) return false;
            FDelegateEditorBinding Key;
            Key.ObjectName = WidgetName;
            Key.PropertyName = FName(*PropertyName);
            if (Blueprint->Bindings.Remove(Key) == 0)
            {
                OutError = CodexUmgError(TEXT("UmgBindingNotFound"), TEXT("The requested binding does not exist."), Blueprint,
                    Callsite, OperationIndex, WidgetName, FString(), TArray<FString>{PropertyName});
                return false;
            }
            bStructural = true;
        }
        else if (Type == TEXT("navigation.set") || Type == TEXT("navigation.clear"))
        {
            FString WidgetName, Direction;
            if (!CodexUmgRequireString(Operation, TEXT("widget"), WidgetName, OutError, Blueprint, OperationIndex, Callsite)
                || !CodexUmgRequireString(Operation, TEXT("direction"), Direction, OutError, Blueprint, OperationIndex, Callsite)) return false;
            UWidget* Widget = CodexUmgFindWidget(Blueprint, WidgetName, OutError, OperationIndex, Callsite);
            if (!Widget) return false;
            if (!CodexUmgModifyObject(Context, Widget, ModifyError, OutError, Blueprint, OperationIndex, WidgetName)) return false;
            if (!Widget->Navigation) Widget->Navigation = NewObject<UWidgetNavigation>(Widget, NAME_None, RF_Transactional);
            if (!Widget->Navigation || !CodexUmgModifyObject(Context, Widget->Navigation, ModifyError, OutError,
                Blueprint, OperationIndex, WidgetName)) return false;
            static const TMap<FString, EUINavigation> Directions = {
                {TEXT("up"), EUINavigation::Up}, {TEXT("down"), EUINavigation::Down}, {TEXT("left"), EUINavigation::Left},
                {TEXT("right"), EUINavigation::Right}, {TEXT("next"), EUINavigation::Next}, {TEXT("previous"), EUINavigation::Previous}};
            const EUINavigation* Nav = Directions.Find(Direction.ToLower());
            if (!Nav)
            {
                OutError = CodexUmgError(TEXT("UmgInvalidNavigationDirection"), TEXT("Unknown navigation direction."),
                    Blueprint, Callsite, OperationIndex, WidgetName);
                return false;
            }
            FWidgetNavigationData& Data = Widget->Navigation->GetNavigationData(*Nav);
            if (Type == TEXT("navigation.clear"))
            {
                Data.Rule = EUINavigationRule::Escape;
                Data.WidgetToFocus = NAME_None;
            }
            else
            {
                FString Rule;
                if (!CodexUmgRequireString(Operation, TEXT("rule"), Rule, OutError, Blueprint, OperationIndex, Callsite)) return false;
                static const TMap<FString, EUINavigationRule> Rules = {
                    {TEXT("escape"), EUINavigationRule::Escape}, {TEXT("explicit"), EUINavigationRule::Explicit},
                    {TEXT("wrap"), EUINavigationRule::Wrap}, {TEXT("stop"), EUINavigationRule::Stop},
                    {TEXT("custom"), EUINavigationRule::Custom}, {TEXT("customboundary"), EUINavigationRule::CustomBoundary}};
                const EUINavigationRule* RuleValue = Rules.Find(Rule.ToLower());
                if (!RuleValue)
                {
                    OutError = CodexUmgError(TEXT("UmgInvalidNavigationRule"), TEXT("Unknown navigation rule."), Blueprint,
                        Callsite, OperationIndex, WidgetName);
                    return false;
                }
                FString Target;
                Operation->TryGetStringField(TEXT("target"), Target);
                if (*RuleValue == EUINavigationRule::Explicit && (Target.IsEmpty() || !Blueprint->WidgetTree->FindWidget(FName(*Target))))
                {
                    OutError = CodexUmgError(TEXT("UmgNavigationTargetNotFound"), TEXT("Explicit navigation requires an existing target widget."),
                        Blueprint, Callsite, OperationIndex, WidgetName, FString(), TArray<FString>{Target});
                    return false;
                }
                if (*RuleValue == EUINavigationRule::Custom || *RuleValue == EUINavigationRule::CustomBoundary)
                {
                    UFunction* Function = Blueprint->SkeletonGeneratedClass
                        ? Blueprint->SkeletonGeneratedClass->FindFunctionByName(FName(*Target), EIncludeSuperFlag::IncludeSuper) : nullptr;
                    UFunction* Signature = FindObject<UFunction>(ANY_PACKAGE, TEXT("CustomWidgetNavigationDelegate__DelegateSignature"));
                    if (Target.IsEmpty() || !Function || !Signature || !Function->IsSignatureCompatibleWith(Signature))
                    {
                        OutError = CodexUmgError(TEXT("UmgNavigationFunctionInvalid"),
                            TEXT("Custom navigation requires 'target' to name a function matching FCustomWidgetNavigationDelegate."),
                            Blueprint, Callsite, OperationIndex, WidgetName, FString(), TArray<FString>{Target});
                        return false;
                    }
                }
                Data.Rule = *RuleValue;
                Data.WidgetToFocus = FName(*Target);
            }
            Widget->Navigation->PostEditChange();
        }
        else if (Type == TEXT("accessibility.set"))
        {
            FString WidgetName;
            const TSharedPtr<FJsonObject>* Properties = nullptr;
            if (!CodexUmgRequireString(Operation, TEXT("widget"), WidgetName, OutError, Blueprint, OperationIndex, Callsite)
                || !Operation->TryGetObjectField(TEXT("properties"), Properties))
            {
                if (!OutError.IsSet()) OutError = CodexUmgError(TEXT("UmgInvalidArgument"), TEXT("'properties' must be an object."),
                    Blueprint, Callsite, OperationIndex, WidgetName);
                return false;
            }
            UWidget* Widget = CodexUmgFindWidget(Blueprint, WidgetName, OutError, OperationIndex, Callsite);
            if (!Widget || !CodexUmgModifyObject(Context, Widget, ModifyError, OutError, Blueprint, OperationIndex, WidgetName)) return false;
            static const TSet<FString> Allowed = {TEXT("bOverrideAccessibleDefaults"), TEXT("bCanChildrenBeAccessible"),
                TEXT("AccessibleBehavior"), TEXT("AccessibleSummaryBehavior"), TEXT("AccessibleText"), TEXT("AccessibleSummaryText")};
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Properties)->Values)
            {
                if (!Allowed.Contains(Pair.Key))
                {
                    OutError = CodexUmgError(TEXT("UmgAccessibilityPropertyRejected"), TEXT("Only UE accessibility properties are accepted."),
                        Blueprint, Callsite, OperationIndex, WidgetName, FString(), TArray<FString>{Pair.Key});
                    return false;
                }
                FBlueprintOperationError PropertyError;
                if (!FBlueprintTypeSystem::SetPropertyValue(Widget, Pair.Key, Pair.Value, PropertyError, CodexUmgAssetPath(Blueprint), OperationIndex))
                {
                    OutError = CodexUmgError(PropertyError.Code, PropertyError.Message, Blueprint, PropertyError.UECallsite,
                        OperationIndex, WidgetName, FString(), PropertyError.Details);
                    return false;
                }
            }
            Widget->PostEditChange();
        }
        else if (Type == TEXT("animation.add"))
        {
            FString Name;
            if (!CodexUmgRequireString(Operation, TEXT("name"), Name, OutError, Blueprint, OperationIndex, Callsite)) return false;
            FUmgOperationError Ignored;
            for (UWidgetAnimation* Existing : Blueprint->Animations)
                if (Existing && (Existing->GetName() == Name || Existing->GetDisplayLabel() == Name))
                {
                    OutError = CodexUmgError(TEXT("UmgAnimationAlreadyExists"), TEXT("Animation name is already in use."),
                        Blueprint, Callsite, OperationIndex, FString(), Name);
                    return false;
                }
            UWidgetAnimation* Animation = NewObject<UWidgetAnimation>(Blueprint, FName(*Name), RF_Transactional);
            if (Animation)
                Animation->MovieScene = NewObject<UMovieScene>(Animation, FName(*Name), RF_Transactional);
            if (!Animation || !Animation->MovieScene)
            {
                OutError = CodexUmgError(TEXT("UmgAnimationConstructionFailed"), TEXT("UE failed to construct the WidgetAnimation MovieScene."),
                    Blueprint, TEXT("NewObject<UWidgetAnimation>/NewObject<UMovieScene>"), OperationIndex, FString(), Name);
                return false;
            }
            if (!CodexUmgModifyObject(Context, Animation, ModifyError, OutError, Blueprint, OperationIndex, FString(), Name)
                || !CodexUmgModifyObject(Context, Animation->MovieScene, ModifyError, OutError, Blueprint, OperationIndex, FString(), Name)) return false;
            Animation->SetDisplayLabel(Name);
            int32 TickResolution = 24000, DisplayRate = 30, StartFrame = 0, EndFrame = 0;
            if (Operation->HasField(TEXT("tickResolution")) && !CodexUmgRequireInteger(Operation, TEXT("tickResolution"), TickResolution, OutError, Blueprint, OperationIndex, Callsite)) return false;
            if (Operation->HasField(TEXT("displayRate")) && !CodexUmgRequireInteger(Operation, TEXT("displayRate"), DisplayRate, OutError, Blueprint, OperationIndex, Callsite)) return false;
            if (Operation->HasField(TEXT("startFrame")) && !CodexUmgRequireInteger(Operation, TEXT("startFrame"), StartFrame, OutError, Blueprint, OperationIndex, Callsite)) return false;
            if (Operation->HasField(TEXT("endFrame")) && !CodexUmgRequireInteger(Operation, TEXT("endFrame"), EndFrame, OutError, Blueprint, OperationIndex, Callsite)) return false;
            if (TickResolution <= 0 || DisplayRate <= 0 || EndFrame < StartFrame)
            {
                OutError = CodexUmgError(TEXT("UmgInvalidAnimationRange"), TEXT("Rates must be positive and endFrame must not precede startFrame."),
                    Blueprint, Callsite, OperationIndex, FString(), Name);
                return false;
            }
            Animation->MovieScene->SetTickResolutionDirectly(FFrameRate(TickResolution, 1));
            Animation->MovieScene->SetDisplayRate(FFrameRate(DisplayRate, 1));
            Animation->MovieScene->SetPlaybackRange(FFrameNumber(StartFrame), EndFrame - StartFrame);
            Blueprint->Animations.Add(Animation);
            bStructural = true;
            OutResult.Data->SetStringField(TEXT("animation"), Name);
        }
        else if (Type == TEXT("animation.remove") || Type == TEXT("animation.rename"))
        {
            FString Name;
            if (!CodexUmgRequireString(Operation, TEXT("animation"), Name, OutError, Blueprint, OperationIndex, Callsite)) return false;
            UWidgetAnimation* Animation = CodexUmgFindAnimation(Blueprint, Name, OutError, OperationIndex, Callsite);
            if (!Animation || !CodexUmgModifyObject(Context, Animation, ModifyError, OutError, Blueprint, OperationIndex, FString(), Name)) return false;
            if (Animation->MovieScene && !CodexUmgModifyObject(Context, Animation->MovieScene, ModifyError, OutError,
                Blueprint, OperationIndex, FString(), Name)) return false;
            if (Type == TEXT("animation.remove"))
            {
                Blueprint->Animations.Remove(Animation);
                if (!Animation->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_ForceNoResetLoaders))
                {
                    OutError = CodexUmgError(TEXT("UmgAnimationRemoveFailed"), TEXT("UE could not move the removed animation out of the Blueprint."),
                        Blueprint, TEXT("UObject::Rename"), OperationIndex, FString(), Name);
                    return false;
                }
            }
            else
            {
                FString NewName;
                if (!CodexUmgRequireString(Operation, TEXT("newName"), NewName, OutError, Blueprint, OperationIndex, Callsite)) return false;
                for (UWidgetAnimation* Existing : Blueprint->Animations)
                    if (Existing != Animation && Existing && (Existing->GetName() == NewName || Existing->GetDisplayLabel() == NewName))
                    {
                        OutError = CodexUmgError(TEXT("UmgAnimationAlreadyExists"), TEXT("Animation name is already in use."),
                            Blueprint, Callsite, OperationIndex, FString(), NewName);
                        return false;
                    }
                const FName OldName = Animation->GetFName();
                FBlueprintEditorUtils::ReplaceVariableReferences(Blueprint, OldName, FName(*NewName));
                Animation->SetDisplayLabel(NewName);
                if (!Animation->Rename(*NewName, Blueprint, REN_DontCreateRedirectors | REN_ForceNoResetLoaders))
                {
                    OutError = CodexUmgError(TEXT("UmgAnimationRenameFailed"), TEXT("UObject::Rename rejected the animation name."),
                        Blueprint, TEXT("UObject::Rename"), OperationIndex, FString(), Name);
                    return false;
                }
                if (Animation->MovieScene) Animation->MovieScene->Rename(*NewName, Animation, REN_DontCreateRedirectors | REN_ForceNoResetLoaders);
            }
            bStructural = true;
        }
        else if (Type == TEXT("animation.binding.add") || Type == TEXT("animation.binding.remove"))
        {
            UWidgetAnimation* Animation = nullptr; UMovieScene* MovieScene = nullptr; FMovieSceneBinding* Binding = nullptr;
            UMovieSceneTrack* Track = nullptr; UMovieSceneSection* Section = nullptr;
            if (!CodexUmgResolveAnimationObjects(Blueprint, Operation, Animation, MovieScene, Binding, Track, Section, OutError,
                OperationIndex, Callsite, Type == TEXT("animation.binding.remove"), false, false)) return false;
            if (!CodexUmgModifyObject(Context, Animation, ModifyError, OutError, Blueprint, OperationIndex,
                FString(), Animation->GetDisplayLabel())
                || !CodexUmgModifyObject(Context, MovieScene, ModifyError, OutError, Blueprint, OperationIndex,
                    FString(), Animation->GetDisplayLabel())) return false;
            if (Type == TEXT("animation.binding.remove"))
            {
                FString GuidText; Operation->TryGetStringField(TEXT("bindingGuid"), GuidText); FGuid Guid; CodexUmgParseGuid(GuidText, Guid);
                FWidgetAnimationBinding* AnimationBinding = CodexUmgFindAnimationBinding(Animation, Guid);
                if (!AnimationBinding)
                {
                    OutError = CodexUmgError(TEXT("UmgAnimationBindingNotFound"), TEXT("WidgetAnimation binding metadata was not found."),
                        Blueprint, Callsite, OperationIndex, FString(), Animation->GetDisplayLabel(), TArray<FString>{GuidText});
                    return false;
                }
                Animation->AnimationBindings.RemoveSingle(*AnimationBinding);
                MovieScene->RemovePossessable(Guid);
            }
            else
            {
                FString WidgetName, Target(TEXT("widget"));
                if (!CodexUmgRequireString(Operation, TEXT("widget"), WidgetName, OutError, Blueprint, OperationIndex, Callsite)) return false;
                UWidget* Widget = CodexUmgFindWidget(Blueprint, WidgetName, OutError, OperationIndex, Callsite);
                if (!Widget) return false;
                Operation->TryGetStringField(TEXT("target"), Target);
                UObject* PossessedObject = Widget;
                if (Target == TEXT("slot"))
                {
                    PossessedObject = Widget->Slot;
                    if (!PossessedObject)
                    {
                        OutError = CodexUmgError(TEXT("UmgPanelSlotNotFound"), TEXT("Slot animation target requires a widget with a PanelSlot."),
                            Blueprint, Callsite, OperationIndex, WidgetName);
                        return false;
                    }
                }
                else if (Target != TEXT("widget"))
                {
                    OutError = CodexUmgError(TEXT("UmgInvalidAnimationTarget"), TEXT("Animation binding target must be 'widget' or 'slot'."),
                        Blueprint, Callsite, OperationIndex, WidgetName, FString(), TArray<FString>{Target});
                    return false;
                }
                const FName SlotObjectName = Target == TEXT("slot") ? PossessedObject->GetFName() : NAME_None;
                if (Animation->AnimationBindings.ContainsByPredicate([&](const FWidgetAnimationBinding& Existing)
                    { return Existing.WidgetName == Widget->GetFName() && Existing.SlotWidgetName == SlotObjectName; }))
                {
                    OutError = CodexUmgError(TEXT("UmgAnimationBindingAlreadyExists"), TEXT("This widget animation target is already bound."),
                        Blueprint, Callsite, OperationIndex, WidgetName, Animation->GetDisplayLabel(), TArray<FString>{Target});
                    return false;
                }
                const FGuid Guid = MovieScene->AddPossessable(PossessedObject->GetName(), PossessedObject->GetClass());
                FWidgetAnimationBinding NewBinding;
                NewBinding.WidgetName = Widget->GetFName();
                NewBinding.SlotWidgetName = SlotObjectName;
                NewBinding.AnimationGuid = Guid;
                // Designer root widgets are still resolved from WidgetTree; bIsRootWidget denotes the UUserWidget itself.
                NewBinding.bIsRootWidget = false;
                Animation->AnimationBindings.Add(NewBinding);
                OutResult.Data->SetStringField(TEXT("bindingGuid"), Guid.ToString(EGuidFormats::DigitsWithHyphens));
            }
            bStructural = true;
        }
        else if (Type == TEXT("animation.track.add") || Type == TEXT("animation.track.remove")
            || Type == TEXT("animation.section.add") || Type == TEXT("animation.section.remove")
            || Type == TEXT("animation.section.set") || Type.StartsWith(TEXT("animation.key.")))
        {
            const bool bNeedTrack = Type != TEXT("animation.track.add");
            const bool bNeedSection = Type == TEXT("animation.section.remove") || Type == TEXT("animation.section.set")
                || Type.StartsWith(TEXT("animation.key."));
            UWidgetAnimation* Animation = nullptr; UMovieScene* MovieScene = nullptr; FMovieSceneBinding* Binding = nullptr;
            UMovieSceneTrack* Track = nullptr; UMovieSceneSection* Section = nullptr;
            if (!CodexUmgResolveAnimationObjects(Blueprint, Operation, Animation, MovieScene, Binding, Track, Section, OutError,
                OperationIndex, Callsite, true, bNeedTrack, bNeedSection)) return false;
            if (!CodexUmgModifyObject(Context, Animation, ModifyError, OutError, Blueprint, OperationIndex,
                FString(), Animation->GetDisplayLabel())
                || !CodexUmgModifyObject(Context, MovieScene, ModifyError, OutError, Blueprint, OperationIndex,
                    FString(), Animation->GetDisplayLabel())) return false;
            if (Track && !CodexUmgModifyObject(Context, Track, ModifyError, OutError, Blueprint, OperationIndex,
                FString(), Animation->GetDisplayLabel())) return false;
            if (Section && !CodexUmgModifyObject(Context, Section, ModifyError, OutError, Blueprint, OperationIndex,
                FString(), Animation->GetDisplayLabel())) return false;
            if (Type == TEXT("animation.track.add"))
            {
                FString ClassPath;
                if (!CodexUmgRequireString(Operation, TEXT("classPath"), ClassPath, OutError, Blueprint, OperationIndex, Callsite)) return false;
                UClass* TrackClass = LoadObject<UClass>(nullptr, *ClassPath);
                if (!TrackClass || !TrackClass->IsChildOf(UMovieSceneTrack::StaticClass()) || TrackClass->HasAnyClassFlags(CLASS_Abstract)
                    || Animation->IsTrackSupported(TrackClass) == ETrackSupport::NotSupported)
                {
                    OutError = CodexUmgError(TEXT("UmgInvalidTrackClass"),
                        TEXT("'classPath' is not a concrete track class supported by UWidgetAnimation."),
                        Blueprint, TEXT("UWidgetAnimation::IsTrackSupported"), OperationIndex,
                        FString(), Animation->GetDisplayLabel(), TArray<FString>{ClassPath});
                    return false;
                }
                Track = MovieScene->AddTrack(TrackClass, Binding->GetObjectGuid());
                if (!Track)
                {
                    OutError = CodexUmgError(TEXT("UmgTrackAddFailed"), TEXT("UMovieScene::AddTrack rejected the track class."),
                        Blueprint, TEXT("UMovieScene::AddTrack"), OperationIndex, FString(), Animation->GetDisplayLabel());
                    return false;
                }
                if (!CodexUmgModifyObject(Context, Track, ModifyError, OutError, Blueprint, OperationIndex,
                    FString(), Animation->GetDisplayLabel())) return false;
                FString PropertyName, PropertyPath;
                if (Operation->TryGetStringField(TEXT("propertyName"), PropertyName))
                {
                    Operation->TryGetStringField(TEXT("propertyPath"), PropertyPath);
                    if (PropertyPath.IsEmpty()) PropertyPath = PropertyName;
                    UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(Track);
                    if (!PropertyTrack || PropertyName.IsEmpty())
                    {
                        OutError = CodexUmgError(TEXT("UmgTrackPropertyMismatch"), TEXT("propertyName requires a UMovieScenePropertyTrack."),
                            Blueprint, Callsite, OperationIndex, FString(), Animation->GetDisplayLabel());
                        return false;
                    }
                    PropertyTrack->SetPropertyNameAndPath(FName(*PropertyName), PropertyPath);
                }
                OutResult.Data->SetNumberField(TEXT("trackIndex"), Binding->GetTracks().IndexOfByKey(Track));
            }
            else if (Type == TEXT("animation.track.remove"))
            {
                if (!MovieScene->RemoveTrack(*Track))
                {
                    OutError = CodexUmgError(TEXT("UmgTrackRemoveFailed"), TEXT("UMovieScene::RemoveTrack rejected the track."),
                        Blueprint, TEXT("UMovieScene::RemoveTrack"), OperationIndex, FString(), Animation->GetDisplayLabel());
                    return false;
                }
            }
            else if (Type == TEXT("animation.section.add"))
            {
                Section = Track->CreateNewSection();
                if (!Section || !Track->SupportsType(Section->GetClass()))
                {
                    OutError = CodexUmgError(TEXT("UmgSectionAddFailed"), TEXT("Track could not create a supported section."),
                        Blueprint, TEXT("UMovieSceneTrack::CreateNewSection"), OperationIndex, FString(), Animation->GetDisplayLabel());
                    return false;
                }
                if (!CodexUmgModifyObject(Context, Section, ModifyError, OutError, Blueprint, OperationIndex,
                    FString(), Animation->GetDisplayLabel())) return false;
                int32 StartFrame = 0, EndFrame = 0;
                if (!CodexUmgRequireInteger(Operation, TEXT("startFrame"), StartFrame, OutError, Blueprint, OperationIndex, Callsite)
                    || !CodexUmgRequireInteger(Operation, TEXT("endFrame"), EndFrame, OutError, Blueprint, OperationIndex, Callsite)) return false;
                if (EndFrame < StartFrame)
                {
                    OutError = CodexUmgError(TEXT("UmgInvalidSectionRange"), TEXT("endFrame must not precede startFrame."),
                        Blueprint, Callsite, OperationIndex, FString(), Animation->GetDisplayLabel());
                    return false;
                }
                Section->SetRange(TRange<FFrameNumber>(FFrameNumber(StartFrame), FFrameNumber(EndFrame)));
                Track->AddSection(*Section);
                OutResult.Data->SetNumberField(TEXT("sectionIndex"), Track->GetAllSections().IndexOfByKey(Section));
            }
            else if (Type == TEXT("animation.section.remove")) Track->RemoveSection(*Section);
            else if (Type == TEXT("animation.section.set"))
            {
                int32 StartFrame = 0, EndFrame = 0;
                if (!CodexUmgRequireInteger(Operation, TEXT("startFrame"), StartFrame, OutError, Blueprint, OperationIndex, Callsite)
                    || !CodexUmgRequireInteger(Operation, TEXT("endFrame"), EndFrame, OutError, Blueprint, OperationIndex, Callsite)) return false;
                if (EndFrame < StartFrame)
                {
                    OutError = CodexUmgError(TEXT("UmgInvalidSectionRange"), TEXT("endFrame must not precede startFrame."),
                        Blueprint, Callsite, OperationIndex, FString(), Animation->GetDisplayLabel());
                    return false;
                }
                Section->SetRange(TRange<FFrameNumber>(FFrameNumber(StartFrame), FFrameNumber(EndFrame)));
                int32 RowIndex = Section->GetRowIndex();
                if (Operation->HasField(TEXT("rowIndex")) && !CodexUmgRequireInteger(Operation, TEXT("rowIndex"), RowIndex, OutError, Blueprint, OperationIndex, Callsite)) return false;
                Section->SetRowIndex(RowIndex);
                bool bActive = Section->IsActive(), bLocked = Section->IsLocked();
                Operation->TryGetBoolField(TEXT("active"), bActive); Operation->TryGetBoolField(TEXT("locked"), bLocked);
                Section->SetIsActive(bActive); Section->SetIsLocked(bLocked);
            }
            else
            {
                FString ChannelType;
                int32 ChannelIndex = INDEX_NONE, Frame = 0;
                if (!CodexUmgRequireString(Operation, TEXT("channelType"), ChannelType, OutError, Blueprint, OperationIndex, Callsite)
                    || !CodexUmgRequireInteger(Operation, TEXT("channelIndex"), ChannelIndex, OutError, Blueprint, OperationIndex, Callsite)
                    || !CodexUmgRequireInteger(Operation, TEXT("frame"), Frame, OutError, Blueprint, OperationIndex, Callsite)) return false;
                FMovieSceneChannel* Channel = Section->GetChannelProxy().GetChannel(FName(*ChannelType), ChannelIndex);
                if (!Channel)
                {
                    TArray<FString> Candidates;
                    for (const FMovieSceneChannelEntry& Entry : Section->GetChannelProxy().GetAllEntries())
                        Candidates.Add(FString::Printf(TEXT("%s[%d]"), *Entry.GetChannelTypeName().ToString(), Entry.GetChannels().Num()));
                    OutError = CodexUmgError(TEXT("UmgAnimationChannelNotFound"), TEXT("The requested channel type/index was not found."),
                        Blueprint, Callsite, OperationIndex, FString(), Animation->GetDisplayLabel(), Candidates);
                    return false;
                }
                const FString Action = Type.RightChop(14);
                int32 NewFrame = Frame;
                if (Action == TEXT("update") && Operation->HasField(TEXT("newFrame"))
                    && !CodexUmgRequireInteger(Operation, TEXT("newFrame"), NewFrame, OutError, Blueprint, OperationIndex, Callsite)) return false;
                TSharedPtr<FJsonValue> Value;
                if (Action != TEXT("remove") && !CodexUmgRequireValue(Operation, TEXT("value"), Value, OutError, Blueprint, OperationIndex, Callsite)) return false;
                if (FName(*ChannelType) == FMovieSceneFloatChannel::StaticStruct()->GetFName())
                {
                    double Number = 0.0;
                    if (Action != TEXT("remove") && (!Value.IsValid() || !Value->TryGetNumber(Number)))
                    { OutError = CodexUmgError(TEXT("UmgAnimationKeyTypeMismatch"), TEXT("Float channel requires a number."), Blueprint, Callsite, OperationIndex); return false; }
                    FMovieSceneFloatValue FloatValue(static_cast<float>(Number));
                    FString Interpolation;
                    if (Operation->TryGetStringField(TEXT("interpolation"), Interpolation))
                    {
                        Interpolation = Interpolation.ToLower();
                        if (Interpolation != TEXT("constant") && Interpolation != TEXT("linear") && Interpolation != TEXT("cubic"))
                        {
                            OutError = CodexUmgError(TEXT("UmgAnimationInterpolationInvalid"),
                                TEXT("'interpolation' must be 'constant', 'linear', or 'cubic'."), Blueprint,
                                Callsite, OperationIndex, FString(), Animation->GetDisplayLabel(), TArray<FString>{Interpolation});
                            return false;
                        }
                        FloatValue.InterpMode = Interpolation == TEXT("constant") ? RCIM_Constant
                            : Interpolation == TEXT("linear") ? RCIM_Linear : RCIM_Cubic;
                    }
                    if (!CodexUmgMutateTypedChannel(static_cast<FMovieSceneFloatChannel*>(Channel), Action, Frame, NewFrame, FloatValue,
                        OutError, Blueprint, OperationIndex, Animation->GetDisplayLabel())) return false;
                }
                else if (FName(*ChannelType) == FMovieSceneBoolChannel::StaticStruct()->GetFName())
                {
                    bool TypedValue = false;
                    if (Action != TEXT("remove") && (!Value.IsValid() || !Value->TryGetBool(TypedValue)))
                    { OutError = CodexUmgError(TEXT("UmgAnimationKeyTypeMismatch"), TEXT("Bool channel requires a boolean."), Blueprint, Callsite, OperationIndex); return false; }
                    if (!CodexUmgMutateTypedChannel(static_cast<FMovieSceneBoolChannel*>(Channel), Action, Frame, NewFrame, TypedValue,
                        OutError, Blueprint, OperationIndex, Animation->GetDisplayLabel())) return false;
                }
                else if (FName(*ChannelType) == FMovieSceneByteChannel::StaticStruct()->GetFName())
                {
                    double Number = 0.0;
                    if (Action != TEXT("remove") && (!Value.IsValid() || !Value->TryGetNumber(Number) || Number < 0 || Number > 255 || Number != FMath::RoundToDouble(Number)))
                    { OutError = CodexUmgError(TEXT("UmgAnimationKeyTypeMismatch"), TEXT("Byte channel requires an integer from 0 to 255."), Blueprint, Callsite, OperationIndex); return false; }
                    if (!CodexUmgMutateTypedChannel(static_cast<FMovieSceneByteChannel*>(Channel), Action, Frame, NewFrame, static_cast<uint8>(Number),
                        OutError, Blueprint, OperationIndex, Animation->GetDisplayLabel())) return false;
                }
                else if (FName(*ChannelType) == FMovieSceneIntegerChannel::StaticStruct()->GetFName())
                {
                    double Number = 0.0;
                    if (Action != TEXT("remove") && (!Value.IsValid() || !Value->TryGetNumber(Number)
                        || Number != FMath::RoundToDouble(Number) || Number < static_cast<double>(MIN_int32)
                        || Number > static_cast<double>(MAX_int32)))
                    { OutError = CodexUmgError(TEXT("UmgAnimationKeyTypeMismatch"), TEXT("Integer channel requires an int32."), Blueprint, Callsite, OperationIndex); return false; }
                    if (!CodexUmgMutateTypedChannel(static_cast<FMovieSceneIntegerChannel*>(Channel), Action, Frame, NewFrame, static_cast<int32>(Number),
                        OutError, Blueprint, OperationIndex, Animation->GetDisplayLabel())) return false;
                }
                else if (FName(*ChannelType) == FMovieSceneStringChannel::StaticStruct()->GetFName())
                {
                    FString TypedValue;
                    if (Action != TEXT("remove") && (!Value.IsValid() || !Value->TryGetString(TypedValue)))
                    { OutError = CodexUmgError(TEXT("UmgAnimationKeyTypeMismatch"), TEXT("String channel requires a string."), Blueprint, Callsite, OperationIndex); return false; }
                    if (!CodexUmgMutateTypedChannel(static_cast<FMovieSceneStringChannel*>(Channel), Action, Frame, NewFrame, TypedValue,
                        OutError, Blueprint, OperationIndex, Animation->GetDisplayLabel())) return false;
                }
                else
                {
                    OutError = CodexUmgError(TEXT("UmgAnimationChannelUnsupported"), TEXT("Channel key editing supports float, bool, byte, integer, and string UE channels."),
                        Blueprint, Callsite, OperationIndex, FString(), Animation->GetDisplayLabel(), TArray<FString>{ChannelType});
                    return false;
                }
            }
        }
        else
        {
            OutError = CodexUmgError(TEXT("UmgUnknownOperation"), FString::Printf(TEXT("Unknown UMG operation '%s'."), *Type),
                Blueprint, Callsite, OperationIndex);
            return false;
        }

        if (bStructural) FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        else FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        Context.MarkPackageChanged(Blueprint->GetOutermost());
        OutResult.bChanged = true;
        return true;
    }

    bool FBlueprintUmgOperations::Inspect(UWidgetBlueprint* Blueprint, TSharedRef<FJsonObject>& OutSnapshot,
        FUmgOperationError& OutError)
    {
        const FString Callsite(TEXT("FBlueprintUmgOperations::Inspect"));
        if (!IsInGameThread())
        {
            OutError = CodexUmgError(TEXT("UmgWrongThread"), TEXT("UMG inspection must run on the game thread."), Blueprint, Callsite, INDEX_NONE);
            return false;
        }
        if (!Blueprint || !Blueprint->WidgetTree)
        {
            OutError = CodexUmgError(TEXT("UmgInvalidBlueprint"), TEXT("A loaded UWidgetBlueprint with a WidgetTree is required."),
                Blueprint, Callsite, INDEX_NONE);
            return false;
        }
        TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>();
        Snapshot->SetStringField(TEXT("assetPath"), CodexUmgAssetPath(Blueprint));
        Snapshot->SetStringField(TEXT("package"), Blueprint->GetOutermost()->GetName());
        Snapshot->SetStringField(TEXT("generatedClass"), Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : FString());

        TArray<UWidget*> Widgets;
        Blueprint->WidgetTree->GetAllWidgets(Widgets);
        Widgets.Sort([](const UWidget& A, const UWidget& B) { return A.GetName() < B.GetName(); });
        TArray<TSharedPtr<FJsonValue>> WidgetJson;
        for (UWidget* Widget : Widgets)
        {
            if (!Widget) continue;
            TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
            Json->SetStringField(TEXT("name"), Widget->GetName());
            Json->SetStringField(TEXT("classPath"), Widget->GetClass()->GetPathName());
            Json->SetBoolField(TEXT("isVariable"), Widget->bIsVariable);
            Json->SetBoolField(TEXT("isRoot"), Blueprint->WidgetTree->RootWidget == Widget);
            int32 ChildIndex = INDEX_NONE;
            if (UPanelWidget* Parent = UWidgetTree::FindWidgetParent(Widget, ChildIndex))
            {
                Json->SetStringField(TEXT("parent"), Parent->GetName());
                Json->SetNumberField(TEXT("childIndex"), ChildIndex);
            }
            else
            {
                UWidget* Owner = nullptr; FName Slot;
                if (CodexUmgFindNamedSlotOwner(Blueprint, Widget, Owner, Slot))
                {
                    Json->SetStringField(TEXT("parent"), Owner->GetName());
                    Json->SetStringField(TEXT("namedSlot"), Slot.ToString());
                }
            }
            Json->SetObjectField(TEXT("properties"), CodexUmgPropertySnapshot(Widget, Blueprint));
            if (Widget->Slot) Json->SetObjectField(TEXT("slotProperties"), CodexUmgPropertySnapshot(Widget->Slot, Blueprint));
            if (Widget->Navigation) Json->SetObjectField(TEXT("navigation"), CodexUmgPropertySnapshot(Widget->Navigation, Blueprint));
            if (Widget->bIsVariable && Blueprint->GeneratedClass)
            {
                FObjectPropertyBase* GeneratedProperty = FindFProperty<FObjectPropertyBase>(Blueprint->GeneratedClass, Widget->GetFName());
                Json->SetBoolField(TEXT("generatedVariableExists"), GeneratedProperty != nullptr);
                if (GeneratedProperty) Json->SetStringField(TEXT("generatedVariableClass"), GeneratedProperty->PropertyClass->GetPathName());
            }
            WidgetJson.Add(MakeShared<FJsonValueObject>(Json));
        }
        Snapshot->SetArrayField(TEXT("widgets"), WidgetJson);

        TArray<TSharedPtr<FJsonValue>> BindingJson;
        TArray<FDelegateEditorBinding> Bindings = Blueprint->Bindings;
        Bindings.Sort([](const FDelegateEditorBinding& A, const FDelegateEditorBinding& B)
        {
            return A.ObjectName == B.ObjectName ? A.PropertyName.LexicalLess(B.PropertyName) : A.ObjectName < B.ObjectName;
        });
        for (const FDelegateEditorBinding& Binding : Bindings)
        {
            TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
            Json->SetStringField(TEXT("widget"), Binding.ObjectName);
            Json->SetStringField(TEXT("property"), Binding.PropertyName.ToString());
            Json->SetStringField(TEXT("kind"), Binding.Kind == EBindingKind::Function ? TEXT("function") : TEXT("property"));
            Json->SetStringField(TEXT("source"), Binding.Kind == EBindingKind::Function
                ? Binding.FunctionName.ToString() : Binding.SourceProperty.ToString());
            Json->SetStringField(TEXT("memberGuid"), Binding.MemberGuid.ToString(EGuidFormats::DigitsWithHyphens));
            BindingJson.Add(MakeShared<FJsonValueObject>(Json));
        }
        Snapshot->SetArrayField(TEXT("bindings"), BindingJson);

        TArray<UK2Node_ComponentBoundEvent*> EventNodes;
        FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, EventNodes);
        EventNodes.Sort([](const UK2Node_ComponentBoundEvent& A, const UK2Node_ComponentBoundEvent& B)
        {
            if (A.ComponentPropertyName != B.ComponentPropertyName) return A.ComponentPropertyName.LexicalLess(B.ComponentPropertyName);
            return A.DelegatePropertyName.LexicalLess(B.DelegatePropertyName);
        });
        TArray<TSharedPtr<FJsonValue>> EventJson;
        for (const UK2Node_ComponentBoundEvent* Node : EventNodes)
        {
            if (!Node) continue;
            TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
            Json->SetStringField(TEXT("widget"), Node->ComponentPropertyName.ToString());
            Json->SetStringField(TEXT("event"), Node->DelegatePropertyName.ToString());
            Json->SetStringField(TEXT("nodeGuid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
            Json->SetStringField(TEXT("graph"), Node->GetGraph() ? Node->GetGraph()->GetPathName() : FString());
            EventJson.Add(MakeShared<FJsonValueObject>(Json));
        }
        Snapshot->SetArrayField(TEXT("events"), EventJson);

        TArray<UWidgetAnimation*> Animations = Blueprint->Animations;
        Animations.Sort([](const UWidgetAnimation& A, const UWidgetAnimation& B) { return A.GetName() < B.GetName(); });
        TArray<TSharedPtr<FJsonValue>> AnimationJson;
        for (UWidgetAnimation* Animation : Animations)
        {
            if (!Animation || !Animation->MovieScene) continue;
            UMovieScene* MovieScene = Animation->MovieScene;
            TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
            Json->SetStringField(TEXT("name"), Animation->GetName());
            Json->SetStringField(TEXT("displayLabel"), Animation->GetDisplayLabel());
            Json->SetNumberField(TEXT("tickResolutionNumerator"), MovieScene->GetTickResolution().Numerator);
            Json->SetNumberField(TEXT("tickResolutionDenominator"), MovieScene->GetTickResolution().Denominator);
            Json->SetNumberField(TEXT("displayRateNumerator"), MovieScene->GetDisplayRate().Numerator);
            Json->SetNumberField(TEXT("displayRateDenominator"), MovieScene->GetDisplayRate().Denominator);
            const TRange<FFrameNumber> Playback = MovieScene->GetPlaybackRange();
            Json->SetNumberField(TEXT("startFrame"), Playback.GetLowerBoundValue().Value);
            Json->SetNumberField(TEXT("endFrame"), Playback.GetUpperBoundValue().Value);
            TArray<TSharedPtr<FJsonValue>> BindingsArray;
            TArray<FWidgetAnimationBinding> AnimationBindings = Animation->AnimationBindings;
            AnimationBindings.Sort([](const FWidgetAnimationBinding& A, const FWidgetAnimationBinding& B)
            { return A.AnimationGuid.ToString() < B.AnimationGuid.ToString(); });
            for (const FWidgetAnimationBinding& AnimationBinding : AnimationBindings)
            {
                TSharedRef<FJsonObject> BindingObject = MakeShared<FJsonObject>();
                BindingObject->SetStringField(TEXT("guid"), AnimationBinding.AnimationGuid.ToString(EGuidFormats::DigitsWithHyphens));
                BindingObject->SetStringField(TEXT("widget"), AnimationBinding.WidgetName.ToString());
                BindingObject->SetStringField(TEXT("slotWidget"), AnimationBinding.SlotWidgetName.ToString());
                BindingObject->SetBoolField(TEXT("isRoot"), AnimationBinding.bIsRootWidget);
                TArray<TSharedPtr<FJsonValue>> TracksArray;
                if (const FMovieSceneBinding* SceneBinding = MovieScene->FindBinding(AnimationBinding.AnimationGuid))
                {
                    for (UMovieSceneTrack* Track : SceneBinding->GetTracks())
                    {
                        if (!Track) continue;
                        TSharedRef<FJsonObject> TrackObject = MakeShared<FJsonObject>();
                        TrackObject->SetStringField(TEXT("classPath"), Track->GetClass()->GetPathName());
                        if (const UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(Track))
                        {
                            TrackObject->SetStringField(TEXT("propertyName"), PropertyTrack->GetPropertyName().ToString());
                            TrackObject->SetStringField(TEXT("propertyPath"), PropertyTrack->GetPropertyPath().ToString());
                        }
                        TArray<TSharedPtr<FJsonValue>> SectionsArray;
                        for (UMovieSceneSection* Section : Track->GetAllSections())
                        {
                            if (!Section) continue;
                            TSharedRef<FJsonObject> SectionObject = MakeShared<FJsonObject>();
                            SectionObject->SetStringField(TEXT("classPath"), Section->GetClass()->GetPathName());
                            const TRange<FFrameNumber> Range = Section->GetRange();
                            if (Range.HasLowerBound()) SectionObject->SetNumberField(TEXT("startFrame"), Range.GetLowerBoundValue().Value);
                            if (Range.HasUpperBound()) SectionObject->SetNumberField(TEXT("endFrame"), Range.GetUpperBoundValue().Value);
                            SectionObject->SetNumberField(TEXT("rowIndex"), Section->GetRowIndex());
                            SectionObject->SetBoolField(TEXT("active"), Section->IsActive());
                            SectionObject->SetBoolField(TEXT("locked"), Section->IsLocked());
                            TArray<TSharedPtr<FJsonValue>> ChannelsArray;
                            for (const FMovieSceneChannelEntry& Entry : Section->GetChannelProxy().GetAllEntries())
                                for (FMovieSceneChannel* Channel : Entry.GetChannels())
                                    ChannelsArray.Add(MakeShared<FJsonValueObject>(CodexUmgChannelSnapshot(Channel, Entry.GetChannelTypeName())));
                            SectionObject->SetArrayField(TEXT("channels"), ChannelsArray);
                            SectionsArray.Add(MakeShared<FJsonValueObject>(SectionObject));
                        }
                        TrackObject->SetArrayField(TEXT("sections"), SectionsArray);
                        TracksArray.Add(MakeShared<FJsonValueObject>(TrackObject));
                    }
                }
                BindingObject->SetArrayField(TEXT("tracks"), TracksArray);
                BindingsArray.Add(MakeShared<FJsonValueObject>(BindingObject));
            }
            Json->SetArrayField(TEXT("bindings"), BindingsArray);
            AnimationJson.Add(MakeShared<FJsonValueObject>(Json));
        }
        Snapshot->SetArrayField(TEXT("animations"), AnimationJson);
        const FString Hash = CodexUmgSha1(CodexUmgCanonicalObject(Snapshot));
        Snapshot->SetStringField(TEXT("snapshotHash"), Hash);
        OutSnapshot = Snapshot;
        return true;
    }

    bool FBlueprintUmgOperations::VerifySnapshot(UWidgetBlueprint* Blueprint, const TSharedRef<FJsonObject>& Expected,
        TSharedRef<FJsonObject>& OutActual, FUmgOperationError& OutError)
    {
        if (!Inspect(Blueprint, OutActual, OutError)) return false;
        FString ExpectedHash;
        if (Expected->TryGetStringField(TEXT("snapshotHash"), ExpectedHash))
        {
            FString ActualHash;
            OutActual->TryGetStringField(TEXT("snapshotHash"), ActualHash);
            if (ExpectedHash.Equals(ActualHash, ESearchCase::IgnoreCase)) return true;
            OutError = CodexUmgError(TEXT("UmgSnapshotMismatch"), TEXT("The persisted Widget Blueprint snapshot does not match the expected hash."),
                Blueprint, TEXT("FBlueprintUmgOperations::VerifySnapshot"), INDEX_NONE, FString(), FString(), TArray<FString>{ExpectedHash, ActualHash});
            return false;
        }
        TSharedRef<FJsonObject> ActualWithoutHash = MakeShared<FJsonObject>();
        ActualWithoutHash->Values = OutActual->Values;
        ActualWithoutHash->RemoveField(TEXT("snapshotHash"));
        TSharedRef<FJsonObject> ExpectedWithoutHash = MakeShared<FJsonObject>();
        ExpectedWithoutHash->Values = Expected->Values;
        ExpectedWithoutHash->RemoveField(TEXT("snapshotHash"));
        const FString ExpectedCanonical = CodexUmgCanonicalObject(ExpectedWithoutHash);
        const FString ActualCanonical = CodexUmgCanonicalObject(ActualWithoutHash);
        if (ExpectedCanonical == ActualCanonical) return true;
        OutError = CodexUmgError(TEXT("UmgSnapshotMismatch"), TEXT("The persisted Widget Blueprint structure differs from the expected snapshot."),
            Blueprint, TEXT("FBlueprintUmgOperations::VerifySnapshot"), INDEX_NONE,
            FString(), FString(), TArray<FString>{CodexUmgSha1(ExpectedCanonical), CodexUmgSha1(ActualCanonical)});
        return false;
    }

    bool FBlueprintUmgOperations::BuildWriteRequest(UWidgetBlueprint* Blueprint,
        const TArray<TSharedRef<FJsonObject>>& Operations, const FString& RequestId,
        const TFunction<bool(const FString&, FString&, FString&)>& StateHashResolver,
        FWritePipelineRequest& OutRequest, FUmgOperationError& OutError)
    {
        if (!Blueprint || !Blueprint->WidgetTree)
        {
            OutError = CodexUmgError(TEXT("UmgInvalidBlueprint"), TEXT("A loaded UWidgetBlueprint with a WidgetTree is required."),
                Blueprint, TEXT("FBlueprintUmgOperations::BuildWriteRequest"), INDEX_NONE);
            return false;
        }
        if (RequestId.TrimStartAndEnd().IsEmpty())
        {
            OutError = CodexUmgError(TEXT("RequestIdRequired"), TEXT("UMG write requests require a non-empty requestId."),
                Blueprint, TEXT("FBlueprintUmgOperations::BuildWriteRequest"), INDEX_NONE);
            return false;
        }
        if (Operations.Num() == 0)
        {
            OutError = CodexUmgError(TEXT("UmgOperationsRequired"), TEXT("At least one UMG operation is required."),
                Blueprint, TEXT("FBlueprintUmgOperations::BuildWriteRequest"), INDEX_NONE);
            return false;
        }
        OutRequest = FWritePipelineRequest();
        OutRequest.RequestId = RequestId;
        OutRequest.TransactionDescription = FString::Printf(TEXT("Codex UMG automation: %s"), *Blueprint->GetPathName());
        OutRequest.StateHashResolver = StateHashResolver;
        OutRequest.Preflight.StateHashResolver = StateHashResolver;
        const FString PackageName = Blueprint->GetOutermost()->GetName();
        OutRequest.Preflight.TargetPackageNames.AddUnique(PackageName);
        OutRequest.Preflight.CompilePackageNames.AddUnique(PackageName);
        if (StateHashResolver)
        {
            FString Hash;
            FString HashError;
            if (!StateHashResolver(PackageName, Hash, HashError))
            {
                OutError = CodexUmgError(TEXT("UmgStateHashFailed"), HashError, Blueprint,
                    TEXT("StateHashResolver"), INDEX_NONE);
                return false;
            }
            OutRequest.Preflight.ExpectedStateHashes.Add(PackageName, Hash);
        }
        for (int32 Index = 0; Index < Operations.Num(); ++Index)
            OutRequest.Operations.Add(MakeShared<FCodexUmgWriteOperation>(Blueprint, Operations[Index], Index, nullptr));
        return true;
    }

    FWritePipelineResult FBlueprintUmgOperations::Execute(UWidgetBlueprint* Blueprint,
        const TArray<TSharedRef<FJsonObject>>& Operations, const FString& RequestId,
        const TFunction<bool(const FString&, FString&, FString&)>& StateHashResolver,
        const FWritePipelineProgress& Progress, FUmgOperationError& OutError)
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
