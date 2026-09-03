#include "CodexUnrealBlueprintTypeSystem.h"

#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

namespace CodexUnrealBlueprint
{
    namespace
    {
        FBlueprintOperationError MakeTypeError(const FString& Code, const FString& Message, const FString& AssetPath,
            const int32 OperationIndex, const FString& Callsite)
        {
            FBlueprintOperationError Error;
            Error.Code = Code;
            Error.Message = Message;
            Error.AssetPath = AssetPath;
            Error.OperationIndex = OperationIndex;
            Error.UECallsite = Callsite;
            return Error;
        }

        bool RequireTypeGameThread(FBlueprintOperationError& OutError, const FString& AssetPath,
            const int32 OperationIndex, const FString& Callsite)
        {
            if (IsInGameThread()) return true;
            OutError = MakeTypeError(TEXT("TYPE_WRONG_THREAD"),
                TEXT("Blueprint type and property operations must run on the game thread."),
                AssetPath, OperationIndex, Callsite);
            return false;
        }

        FBlueprintOperationResult TypeWrongThread(const FString& AssetPath, const int32 OperationIndex,
            const FString& Callsite)
        {
            FBlueprintOperationResult Result;
            Result.Error = MakeTypeError(TEXT("TYPE_WRONG_THREAD"),
                TEXT("Blueprint type and property operations must run on the game thread."),
                AssetPath, OperationIndex, Callsite);
            return Result;
        }

        bool ReadOptionalBool(const TSharedPtr<FJsonObject>& Json, const TCHAR* Field, bool& OutValue,
            FBlueprintOperationError& OutError, const FString& AssetPath, const int32 OperationIndex)
        {
            if (!Json->HasField(Field))
            {
                return true;
            }
            if (!Json->TryGetBoolField(Field, OutValue))
            {
                OutError = MakeTypeError(TEXT("TYPE_INVALID_PIN"), FString::Printf(TEXT("Field '%s' must be boolean."), Field),
                    AssetPath, OperationIndex, TEXT("FBlueprintTypeSystem::ParsePinType"));
                return false;
            }
            return true;
        }

        bool JsonToDefaultText(const TSharedPtr<FJsonValue>& Value, FString& OutText,
            const FEdGraphPinType* PinType = nullptr)
        {
            if (!Value.IsValid() || Value->IsNull())
            {
                OutText.Empty();
                return true;
            }
            switch (Value->Type)
            {
            case EJson::String:
                OutText = Value->AsString();
                return true;
            case EJson::Boolean:
                OutText = Value->AsBool() ? TEXT("true") : TEXT("false");
                return true;
            case EJson::Number:
            {
                const double Number = Value->AsNumber();
                const bool bIntegerPin = PinType != nullptr
                    && (PinType->PinCategory == UEdGraphSchema_K2::PC_Int
                        || PinType->PinCategory == UEdGraphSchema_K2::PC_Int64
                        || PinType->PinCategory == UEdGraphSchema_K2::PC_Byte);
                if (bIntegerPin)
                {
                    if (!FMath::IsFinite(Number) || !FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number)))
                    {
                        return false;
                    }
                    OutText = FString::Printf(TEXT("%lld"), static_cast<int64>(Number));
                }
                else
                {
                    OutText = FString::SanitizeFloat(Number);
                }
                return true;
            }
            case EJson::Array:
            {
                TArray<FString> Items;
                for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
                {
                    FString ItemText;
                    if (!JsonToDefaultText(Item, ItemText))
                    {
                        return false;
                    }
                    Items.Add(ItemText);
                }
                OutText = FString::Printf(TEXT("(%s)"), *FString::Join(Items, TEXT(",")));
                return true;
            }
            case EJson::Object:
            {
                TArray<FString> Fields;
                for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Value->AsObject()->Values)
                {
                    FString FieldText;
                    if (!JsonToDefaultText(Pair.Value, FieldText))
                    {
                        return false;
                    }
                    Fields.Add(FString::Printf(TEXT("%s=%s"), *Pair.Key, *FieldText));
                }
                Fields.Sort();
                OutText = FString::Printf(TEXT("(%s)"), *FString::Join(Fields, TEXT(",")));
                return true;
            }
            default:
                return false;
            }
        }

        bool IsIntegerNumber(const double Number)
        {
            return FMath::IsFinite(Number) && FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number));
        }

        TSharedPtr<FJsonValue> NumberToJson(const double Number)
        {
            return MakeShared<FJsonValueNumber>(Number);
        }
    }

    UObject* FBlueprintTypeSystem::ResolveTypeObject(const FString& ObjectPath, UClass* RequiredClass,
        FBlueprintOperationError& OutError, const FString& AssetPath, const int32 OperationIndex)
    {
        if (ObjectPath.IsEmpty())
        {
            OutError = MakeTypeError(TEXT("TYPE_SUBTYPE_REQUIRED"), TEXT("This pin category requires 'subCategoryObjectPath'."),
                AssetPath, OperationIndex, TEXT("FBlueprintTypeSystem::ResolveTypeObject"));
            return nullptr;
        }
        UObject* Object = StaticLoadObject(RequiredClass, nullptr, *ObjectPath);
        if (Object == nullptr)
        {
            OutError = MakeTypeError(TEXT("TYPE_SUBTYPE_NOT_FOUND"),
                FString::Printf(TEXT("Type object '%s' was not found or is not a %s."), *ObjectPath, *RequiredClass->GetName()),
                AssetPath, OperationIndex, TEXT("FBlueprintTypeSystem::ResolveTypeObject"));
        }
        return Object;
    }

    bool FBlueprintTypeSystem::ParsePinType(const TSharedPtr<FJsonObject>& Json, FEdGraphPinType& OutType,
        FBlueprintOperationError& OutError, const FString& AssetPath, const int32 OperationIndex)
    {
        if (!RequireTypeGameThread(OutError, AssetPath, OperationIndex, TEXT("FBlueprintTypeSystem::ParsePinType"))) return false;
        if (!Json.IsValid())
        {
            OutError = MakeTypeError(TEXT("TYPE_INVALID_PIN"), TEXT("Pin type must be an object."), AssetPath,
                OperationIndex, TEXT("FBlueprintTypeSystem::ParsePinType"));
            return false;
        }
        static const TSet<FString> AllowedFields = {
            TEXT("category"), TEXT("subCategory"), TEXT("subCategoryObjectPath"), TEXT("memberReference"), TEXT("container"),
            TEXT("valueType"), TEXT("isReference"), TEXT("isConst"), TEXT("isWeakPointer"), TEXT("isUObjectWrapper")
        };
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Json->Values)
        {
            if (!AllowedFields.Contains(Field.Key))
            {
                OutError = MakeTypeError(TEXT("TYPE_UNKNOWN_FIELD"), FString::Printf(TEXT("Unknown pin type field '%s'."), *Field.Key),
                    AssetPath, OperationIndex, TEXT("FBlueprintTypeSystem::ParsePinType"));
                return false;
            }
        }

        FString Category;
        if (!Json->TryGetStringField(TEXT("category"), Category) || Category.IsEmpty())
        {
            OutError = MakeTypeError(TEXT("TYPE_CATEGORY_REQUIRED"), TEXT("Pin type requires a non-empty 'category'."),
                AssetPath, OperationIndex, TEXT("FBlueprintTypeSystem::ParsePinType"));
            return false;
        }
        static const TSet<FName> SupportedCategories = {
            UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PC_Boolean, UEdGraphSchema_K2::PC_Byte,
            UEdGraphSchema_K2::PC_Class, UEdGraphSchema_K2::PC_Int, UEdGraphSchema_K2::PC_Int64,
            UEdGraphSchema_K2::PC_Float, UEdGraphSchema_K2::PC_Name, UEdGraphSchema_K2::PC_Delegate,
            UEdGraphSchema_K2::PC_MCDelegate, UEdGraphSchema_K2::PC_Object, UEdGraphSchema_K2::PC_Interface,
            UEdGraphSchema_K2::PC_String, UEdGraphSchema_K2::PC_Text, UEdGraphSchema_K2::PC_Struct,
            UEdGraphSchema_K2::PC_Wildcard, UEdGraphSchema_K2::PC_SoftObject, UEdGraphSchema_K2::PC_SoftClass
        };
        OutType.ResetToDefaults();
        OutType.PinCategory = FName(*Category);
        if (!SupportedCategories.Contains(OutType.PinCategory))
        {
            OutError = MakeTypeError(TEXT("TYPE_CATEGORY_UNSUPPORTED"), FString::Printf(TEXT("Pin category '%s' is unsupported."), *Category),
                AssetPath, OperationIndex, TEXT("FBlueprintTypeSystem::ParsePinType"));
            return false;
        }
        FString SubCategory;
        Json->TryGetStringField(TEXT("subCategory"), SubCategory);
        OutType.PinSubCategory = FName(*SubCategory);

        FString ObjectPath;
        Json->TryGetStringField(TEXT("subCategoryObjectPath"), ObjectPath);
        UClass* RequiredClass = nullptr;
        if (OutType.PinCategory == UEdGraphSchema_K2::PC_Object || OutType.PinCategory == UEdGraphSchema_K2::PC_Class ||
            OutType.PinCategory == UEdGraphSchema_K2::PC_Interface || OutType.PinCategory == UEdGraphSchema_K2::PC_SoftObject ||
            OutType.PinCategory == UEdGraphSchema_K2::PC_SoftClass)
        {
            RequiredClass = UClass::StaticClass();
        }
        else if (OutType.PinCategory == UEdGraphSchema_K2::PC_Struct)
        {
            RequiredClass = UScriptStruct::StaticClass();
        }
        else if (OutType.PinCategory == UEdGraphSchema_K2::PC_Byte && !ObjectPath.IsEmpty())
        {
            RequiredClass = UEnum::StaticClass();
        }
        else if ((OutType.PinCategory == UEdGraphSchema_K2::PC_Delegate || OutType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate) && !ObjectPath.IsEmpty())
        {
            RequiredClass = UFunction::StaticClass();
        }
        if (RequiredClass != nullptr)
        {
            OutType.PinSubCategoryObject = ResolveTypeObject(ObjectPath, RequiredClass, OutError, AssetPath, OperationIndex);
            if (!OutType.PinSubCategoryObject.IsValid())
            {
                return false;
            }
        }
        else if (!ObjectPath.IsEmpty())
        {
            OutError = MakeTypeError(TEXT("TYPE_SUBTYPE_NOT_ALLOWED"), TEXT("This category does not accept 'subCategoryObjectPath'."),
                AssetPath, OperationIndex, TEXT("FBlueprintTypeSystem::ParsePinType"));
            return false;
        }

        if (Json->HasField(TEXT("memberReference")))
        {
            const TSharedPtr<FJsonObject>* ReferenceJson = nullptr;
            if (!Json->TryGetObjectField(TEXT("memberReference"), ReferenceJson) || ReferenceJson == nullptr)
            {
                OutError = MakeTypeError(TEXT("TYPE_MEMBER_REFERENCE_INVALID"), TEXT("Field 'memberReference' must be an object."),
                    AssetPath, OperationIndex, TEXT("FBlueprintTypeSystem::ParsePinType"));
                return false;
            }
            FString ParentPath;
            FString MemberName;
            FString MemberGuid;
            (*ReferenceJson)->TryGetStringField(TEXT("parentPath"), ParentPath);
            (*ReferenceJson)->TryGetStringField(TEXT("name"), MemberName);
            (*ReferenceJson)->TryGetStringField(TEXT("guid"), MemberGuid);
            if (MemberName.IsEmpty())
            {
                OutError = MakeTypeError(TEXT("TYPE_MEMBER_REFERENCE_INVALID"), TEXT("Member reference requires a non-empty 'name'."),
                    AssetPath, OperationIndex, TEXT("FBlueprintTypeSystem::ParsePinType"));
                return false;
            }
            if (!ParentPath.IsEmpty())
            {
                OutType.PinSubCategoryMemberReference.MemberParent = StaticLoadObject(UObject::StaticClass(), nullptr, *ParentPath);
                if (OutType.PinSubCategoryMemberReference.MemberParent == nullptr)
                {
                    OutError = MakeTypeError(TEXT("TYPE_MEMBER_PARENT_NOT_FOUND"), FString::Printf(TEXT("Member parent '%s' was not found."), *ParentPath),
                        AssetPath, OperationIndex, TEXT("FBlueprintTypeSystem::ParsePinType"));
                    return false;
                }
            }
            OutType.PinSubCategoryMemberReference.MemberName = FName(*MemberName);
            if (!MemberGuid.IsEmpty() && !FGuid::Parse(MemberGuid, OutType.PinSubCategoryMemberReference.MemberGuid))
            {
                OutError = MakeTypeError(TEXT("TYPE_MEMBER_GUID_INVALID"), TEXT("Member reference 'guid' is invalid."),
                    AssetPath, OperationIndex, TEXT("FBlueprintTypeSystem::ParsePinType"));
                return false;
            }
        }

        FString Container = TEXT("none");
        if (Json->HasField(TEXT("container")) && !Json->TryGetStringField(TEXT("container"), Container))
        {
            OutError = MakeTypeError(TEXT("TYPE_INVALID_CONTAINER"), TEXT("Field 'container' must be a string."), AssetPath,
                OperationIndex, TEXT("FBlueprintTypeSystem::ParsePinType"));
            return false;
        }
        if (Container == TEXT("none")) OutType.ContainerType = EPinContainerType::None;
        else if (Container == TEXT("array")) OutType.ContainerType = EPinContainerType::Array;
        else if (Container == TEXT("set")) OutType.ContainerType = EPinContainerType::Set;
        else if (Container == TEXT("map")) OutType.ContainerType = EPinContainerType::Map;
        else
        {
            OutError = MakeTypeError(TEXT("TYPE_INVALID_CONTAINER"), FString::Printf(TEXT("Unknown container '%s'."), *Container),
                AssetPath, OperationIndex, TEXT("FBlueprintTypeSystem::ParsePinType"));
            return false;
        }
        if (OutType.ContainerType == EPinContainerType::Map)
        {
            const TSharedPtr<FJsonObject>* ValueTypeJson = nullptr;
            if (!Json->TryGetObjectField(TEXT("valueType"), ValueTypeJson) || ValueTypeJson == nullptr)
            {
                OutError = MakeTypeError(TEXT("TYPE_MAP_VALUE_REQUIRED"), TEXT("Map pin type requires object field 'valueType'."),
                    AssetPath, OperationIndex, TEXT("FBlueprintTypeSystem::ParsePinType"));
                return false;
            }
            FEdGraphPinType ParsedValue;
            if (!ParsePinType(*ValueTypeJson, ParsedValue, OutError, AssetPath, OperationIndex))
            {
                return false;
            }
            if (ParsedValue.IsContainer())
            {
                OutError = MakeTypeError(TEXT("TYPE_NESTED_CONTAINER"), TEXT("Map values cannot be containers in Blueprint."),
                    AssetPath, OperationIndex, TEXT("FBlueprintTypeSystem::ParsePinType"));
                return false;
            }
            OutType.PinValueType.TerminalCategory = ParsedValue.PinCategory;
            OutType.PinValueType.TerminalSubCategory = ParsedValue.PinSubCategory;
            OutType.PinValueType.TerminalSubCategoryObject = ParsedValue.PinSubCategoryObject;
            OutType.PinValueType.bTerminalIsConst = ParsedValue.bIsConst;
            OutType.PinValueType.bTerminalIsWeakPointer = ParsedValue.bIsWeakPointer;
            OutType.PinValueType.bTerminalIsUObjectWrapper = ParsedValue.bIsUObjectWrapper;
        }
        else if (Json->HasField(TEXT("valueType")))
        {
            OutError = MakeTypeError(TEXT("TYPE_MAP_VALUE_NOT_ALLOWED"), TEXT("Only map pin types accept 'valueType'."),
                AssetPath, OperationIndex, TEXT("FBlueprintTypeSystem::ParsePinType"));
            return false;
        }

        bool bReference = false;
        bool bConst = false;
        bool bWeak = false;
        bool bWrapper = false;
        if (!ReadOptionalBool(Json, TEXT("isReference"), bReference, OutError, AssetPath, OperationIndex) ||
            !ReadOptionalBool(Json, TEXT("isConst"), bConst, OutError, AssetPath, OperationIndex) ||
            !ReadOptionalBool(Json, TEXT("isWeakPointer"), bWeak, OutError, AssetPath, OperationIndex) ||
            !ReadOptionalBool(Json, TEXT("isUObjectWrapper"), bWrapper, OutError, AssetPath, OperationIndex))
        {
            return false;
        }
        if (bWeak && OutType.PinCategory != UEdGraphSchema_K2::PC_Object)
        {
            OutError = MakeTypeError(TEXT("TYPE_WEAK_REQUIRES_OBJECT"), TEXT("Weak references require category 'object'."),
                AssetPath, OperationIndex, TEXT("FBlueprintTypeSystem::ParsePinType"));
            return false;
        }
        OutType.bIsReference = bReference;
        OutType.bIsConst = bConst;
        OutType.bIsWeakPointer = bWeak;
        OutType.bIsUObjectWrapper = bWrapper;
        return true;
    }

    TSharedRef<FJsonObject> FBlueprintTypeSystem::PinTypeToJson(const FEdGraphPinType& Type)
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("category"), Type.PinCategory.ToString());
        Json->SetStringField(TEXT("subCategory"), Type.PinSubCategory.ToString());
        if (Type.PinSubCategoryObject.IsValid())
        {
            Json->SetStringField(TEXT("subCategoryObjectPath"), Type.PinSubCategoryObject->GetPathName());
        }
        if (!Type.PinSubCategoryMemberReference.MemberName.IsNone())
        {
            TSharedRef<FJsonObject> Reference = MakeShared<FJsonObject>();
            Reference->SetStringField(TEXT("name"), Type.PinSubCategoryMemberReference.MemberName.ToString());
            if (Type.PinSubCategoryMemberReference.MemberParent)
            {
                Reference->SetStringField(TEXT("parentPath"), Type.PinSubCategoryMemberReference.MemberParent->GetPathName());
            }
            if (Type.PinSubCategoryMemberReference.MemberGuid.IsValid())
            {
                Reference->SetStringField(TEXT("guid"), Type.PinSubCategoryMemberReference.MemberGuid.ToString(EGuidFormats::DigitsWithHyphens));
            }
            Json->SetObjectField(TEXT("memberReference"), Reference);
        }
        const TCHAR* Container = TEXT("none");
        if (Type.IsArray()) Container = TEXT("array");
        else if (Type.IsSet()) Container = TEXT("set");
        else if (Type.IsMap()) Container = TEXT("map");
        Json->SetStringField(TEXT("container"), Container);
        Json->SetBoolField(TEXT("isReference"), Type.bIsReference);
        Json->SetBoolField(TEXT("isConst"), Type.bIsConst);
        Json->SetBoolField(TEXT("isWeakPointer"), Type.bIsWeakPointer);
        Json->SetBoolField(TEXT("isUObjectWrapper"), Type.bIsUObjectWrapper);
        if (Type.IsMap())
        {
            FEdGraphPinType ValueType;
            ValueType.PinCategory = Type.PinValueType.TerminalCategory;
            ValueType.PinSubCategory = Type.PinValueType.TerminalSubCategory;
            ValueType.PinSubCategoryObject = Type.PinValueType.TerminalSubCategoryObject;
            ValueType.bIsConst = Type.PinValueType.bTerminalIsConst;
            ValueType.bIsWeakPointer = Type.PinValueType.bTerminalIsWeakPointer;
            ValueType.bIsUObjectWrapper = Type.PinValueType.bTerminalIsUObjectWrapper;
            Json->SetObjectField(TEXT("valueType"), PinTypeToJson(ValueType));
        }
        return Json;
    }

    bool FBlueprintTypeSystem::ResolvePropertyPath(UObject* Owner, const FString& PropertyPath, FProperty*& OutProperty,
        void*& OutAddress, FBlueprintOperationError& OutError, const FString& AssetPath, const int32 OperationIndex)
    {
        if (Owner == nullptr || PropertyPath.IsEmpty())
        {
            OutError = MakeTypeError(TEXT("PROPERTY_PATH_INVALID"), TEXT("A valid owner and non-empty property path are required."),
                AssetPath, OperationIndex, TEXT("FBlueprintTypeSystem::ResolvePropertyPath"));
            return false;
        }
        TArray<FString> Segments;
        PropertyPath.ParseIntoArray(Segments, TEXT("."), true);
        UStruct* Scope = Owner->GetClass();
        void* Address = Owner;
        for (int32 Index = 0; Index < Segments.Num(); ++Index)
        {
            FProperty* Property = Scope->FindPropertyByName(FName(*Segments[Index]));
            if (Property == nullptr)
            {
                OutError = MakeTypeError(TEXT("PROPERTY_NOT_FOUND"), FString::Printf(TEXT("Property '%s' was not found in '%s'."),
                    *Segments[Index], *Scope->GetPathName()), AssetPath, OperationIndex,
                    TEXT("FBlueprintTypeSystem::ResolvePropertyPath"));
                return false;
            }
            Address = Property->ContainerPtrToValuePtr<void>(Address);
            if (Index == Segments.Num() - 1)
            {
                OutProperty = Property;
                OutAddress = Address;
                return true;
            }
            const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
            if (StructProperty == nullptr)
            {
                OutError = MakeTypeError(TEXT("PROPERTY_PATH_NON_STRUCT"), FString::Printf(TEXT("Property '%s' is not a struct."),
                    *Segments[Index]), AssetPath, OperationIndex, TEXT("FBlueprintTypeSystem::ResolvePropertyPath"));
                return false;
            }
            Scope = StructProperty->Struct;
        }
        return false;
    }

    bool FBlueprintTypeSystem::SetPropertyValue(UObject* Owner, const FString& PropertyPath,
        const TSharedPtr<FJsonValue>& Value, FBlueprintOperationError& OutError, const FString& AssetPath,
        const int32 OperationIndex)
    {
        if (!RequireTypeGameThread(OutError, AssetPath, OperationIndex, TEXT("FBlueprintTypeSystem::SetPropertyValue"))) return false;
        FProperty* Property = nullptr;
        void* Address = nullptr;
        if (!ResolvePropertyPath(Owner, PropertyPath, Property, Address, OutError, AssetPath, OperationIndex))
        {
            return false;
        }
        const bool bBlueprintClassDefault = Owner->HasAnyFlags(RF_ClassDefaultObject)
            && Property->HasAnyPropertyFlags(CPF_BlueprintVisible);
        if ((!Property->HasAnyPropertyFlags(CPF_Edit) && !bBlueprintClassDefault)
            || Property->HasAnyPropertyFlags(CPF_EditConst))
        {
            OutError = MakeTypeError(TEXT("PROPERTY_NOT_EDITABLE"), FString::Printf(TEXT("Property '%s' is not editable."), *PropertyPath),
                AssetPath, OperationIndex, TEXT("FBlueprintTypeSystem::SetPropertyValue"));
            return false;
        }
        Owner->Modify();
        return SetPropertyValue(Property, Address, Value, OutError, AssetPath, OperationIndex);
    }

    bool FBlueprintTypeSystem::SetPropertyValue(FProperty* Property, void* ValueAddress,
        const TSharedPtr<FJsonValue>& Value, FBlueprintOperationError& OutError, const FString& AssetPath,
        const int32 OperationIndex)
    {
        if (!RequireTypeGameThread(OutError, AssetPath, OperationIndex, TEXT("FBlueprintTypeSystem::SetPropertyValue"))) return false;
        const FString Callsite = TEXT("FBlueprintTypeSystem::SetPropertyValue");
        if (Property == nullptr || ValueAddress == nullptr || !Value.IsValid())
        {
            OutError = MakeTypeError(TEXT("PROPERTY_VALUE_INVALID"), TEXT("Property and JSON value are required."),
                AssetPath, OperationIndex, Callsite);
            return false;
        }
        if (Value->IsNull())
        {
            if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
            {
                ObjectProperty->SetObjectPropertyValue(ValueAddress, nullptr);
                return true;
            }
            if (FSoftObjectProperty* SoftProperty = CastField<FSoftObjectProperty>(Property))
            {
                SoftProperty->SetPropertyValue(ValueAddress, FSoftObjectPtr());
                return true;
            }
            OutError = MakeTypeError(TEXT("PROPERTY_NULL_NOT_ALLOWED"), FString::Printf(TEXT("Property '%s' does not accept null."), *Property->GetName()),
                AssetPath, OperationIndex, Callsite);
            return false;
        }
        if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
        {
            if (Value->Type != EJson::Boolean) goto TypeMismatch;
            BoolProperty->SetPropertyValue(ValueAddress, Value->AsBool());
            return true;
        }
        if (FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
        {
            if (NumericProperty->IsEnum())
            {
                UEnum* Enum = NumericProperty->GetIntPropertyEnum();
                int64 EnumValue = INDEX_NONE;
                if (Value->Type == EJson::String)
                {
                    EnumValue = Enum->GetValueByNameString(Value->AsString());
                }
                else if (Value->Type == EJson::Number && IsIntegerNumber(Value->AsNumber()))
                {
                    EnumValue = static_cast<int64>(Value->AsNumber());
                }
                if (!Enum->IsValidEnumValue(EnumValue)) goto TypeMismatch;
                NumericProperty->SetIntPropertyValue(ValueAddress, EnumValue);
                return true;
            }
            if (Value->Type != EJson::Number || !FMath::IsFinite(Value->AsNumber())) goto TypeMismatch;
            if (NumericProperty->IsInteger())
            {
                if (!IsIntegerNumber(Value->AsNumber())) goto TypeMismatch;
                const FString Text = FString::SanitizeFloat(Value->AsNumber(), 0);
                if (NumericProperty->ImportText(*Text, ValueAddress, PPF_None, nullptr) == nullptr) goto TypeMismatch;
            }
            else
            {
                NumericProperty->SetFloatingPointPropertyValue(ValueAddress, Value->AsNumber());
            }
            return true;
        }
        if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
        {
            int64 EnumValue = INDEX_NONE;
            if (Value->Type == EJson::String) EnumValue = EnumProperty->GetEnum()->GetValueByNameString(Value->AsString());
            else if (Value->Type == EJson::Number && IsIntegerNumber(Value->AsNumber())) EnumValue = static_cast<int64>(Value->AsNumber());
            if (!EnumProperty->GetEnum()->IsValidEnumValue(EnumValue)) goto TypeMismatch;
            EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(ValueAddress, EnumValue);
            return true;
        }
        if (FStrProperty* StringProperty = CastField<FStrProperty>(Property))
        {
            if (Value->Type != EJson::String) goto TypeMismatch;
            StringProperty->SetPropertyValue(ValueAddress, Value->AsString());
            return true;
        }
        if (FNameProperty* NameProperty = CastField<FNameProperty>(Property))
        {
            if (Value->Type != EJson::String) goto TypeMismatch;
            NameProperty->SetPropertyValue(ValueAddress, FName(*Value->AsString()));
            return true;
        }
        if (FTextProperty* TextProperty = CastField<FTextProperty>(Property))
        {
            if (Value->Type != EJson::String) goto TypeMismatch;
            TextProperty->SetPropertyValue(ValueAddress, FText::FromString(Value->AsString()));
            return true;
        }
        if (FClassProperty* ClassProperty = CastField<FClassProperty>(Property))
        {
            if (Value->Type != EJson::String) goto TypeMismatch;
            UClass* Class = LoadObject<UClass>(nullptr, *Value->AsString());
            if (Class == nullptr || !Class->IsChildOf(ClassProperty->MetaClass)) goto TypeMismatch;
            ClassProperty->SetObjectPropertyValue(ValueAddress, Class);
            return true;
        }
        if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
        {
            if (Value->Type != EJson::String) goto TypeMismatch;
            UObject* Object = StaticLoadObject(ObjectProperty->PropertyClass, nullptr, *Value->AsString());
            if (Object == nullptr) goto TypeMismatch;
            ObjectProperty->SetObjectPropertyValue(ValueAddress, Object);
            return true;
        }
        if (FSoftClassProperty* SoftClassProperty = CastField<FSoftClassProperty>(Property))
        {
            if (Value->Type != EJson::String) goto TypeMismatch;
            const FSoftObjectPath Path(Value->AsString());
            UClass* LoadedClass = Cast<UClass>(Path.TryLoad());
            if (LoadedClass == nullptr || !LoadedClass->IsChildOf(SoftClassProperty->MetaClass)) goto TypeMismatch;
            SoftClassProperty->SetPropertyValue(ValueAddress, FSoftObjectPtr(Path));
            return true;
        }
        if (FSoftObjectProperty* SoftProperty = CastField<FSoftObjectProperty>(Property))
        {
            if (Value->Type != EJson::String) goto TypeMismatch;
            const FSoftObjectPath Path(Value->AsString());
            if (!Path.IsValid()) goto TypeMismatch;
            SoftProperty->SetPropertyValue(ValueAddress, FSoftObjectPtr(Path));
            return true;
        }
        if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
        {
            if (Value->Type != EJson::Object) goto TypeMismatch;
            const TSharedPtr<FJsonObject> Object = Value->AsObject();
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
            {
                FProperty* ChildProperty = StructProperty->Struct->FindPropertyByName(FName(*Field.Key));
                if (ChildProperty == nullptr)
                {
                    OutError = MakeTypeError(TEXT("PROPERTY_STRUCT_FIELD_NOT_FOUND"),
                        FString::Printf(TEXT("Struct '%s' has no field '%s'."), *StructProperty->Struct->GetPathName(), *Field.Key),
                        AssetPath, OperationIndex, Callsite);
                    return false;
                }
                if (!SetPropertyValue(ChildProperty, ChildProperty->ContainerPtrToValuePtr<void>(ValueAddress), Field.Value,
                    OutError, AssetPath, OperationIndex)) return false;
            }
            return true;
        }
        if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
        {
            if (Value->Type != EJson::Array) goto TypeMismatch;
            FScriptArrayHelper Helper(ArrayProperty, ValueAddress);
            Helper.EmptyValues(Value->AsArray().Num());
            for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
            {
                const int32 Index = Helper.AddValue();
                if (!SetPropertyValue(ArrayProperty->Inner, Helper.GetRawPtr(Index), Item, OutError, AssetPath, OperationIndex)) return false;
            }
            return true;
        }
        if (FSetProperty* SetProperty = CastField<FSetProperty>(Property))
        {
            if (Value->Type != EJson::Array) goto TypeMismatch;
            FScriptSetHelper Helper(SetProperty, ValueAddress);
            Helper.EmptyElements(Value->AsArray().Num());
            for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
            {
                const int32 Index = Helper.AddDefaultValue_Invalid_NeedsRehash();
                if (!SetPropertyValue(SetProperty->ElementProp, Helper.GetElementPtr(Index), Item, OutError, AssetPath, OperationIndex)) return false;
            }
            Helper.Rehash();
            if (Helper.Num() != Value->AsArray().Num())
            {
                OutError = MakeTypeError(TEXT("PROPERTY_SET_DUPLICATE"), TEXT("Set JSON contains duplicate values."),
                    AssetPath, OperationIndex, Callsite);
                return false;
            }
            return true;
        }
        if (FMapProperty* MapProperty = CastField<FMapProperty>(Property))
        {
            if (Value->Type != EJson::Array) goto TypeMismatch;
            FScriptMapHelper Helper(MapProperty, ValueAddress);
            Helper.EmptyValues(Value->AsArray().Num());
            for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
            {
                const TSharedPtr<FJsonObject>* Entry = nullptr;
                if (!Item.IsValid() || !Item->TryGetObject(Entry) || Entry == nullptr || !(*Entry)->HasField(TEXT("key")) || !(*Entry)->HasField(TEXT("value"))) goto TypeMismatch;
                const int32 Index = Helper.AddDefaultValue_Invalid_NeedsRehash();
                if (!SetPropertyValue(MapProperty->KeyProp, Helper.GetKeyPtr(Index), (*Entry)->TryGetField(TEXT("key")), OutError, AssetPath, OperationIndex) ||
                    !SetPropertyValue(MapProperty->ValueProp, Helper.GetValuePtr(Index), (*Entry)->TryGetField(TEXT("value")), OutError, AssetPath, OperationIndex)) return false;
            }
            Helper.Rehash();
            if (Helper.Num() != Value->AsArray().Num())
            {
                OutError = MakeTypeError(TEXT("PROPERTY_MAP_DUPLICATE_KEY"), TEXT("Map JSON contains duplicate keys."),
                    AssetPath, OperationIndex, Callsite);
                return false;
            }
            return true;
        }

        OutError = MakeTypeError(TEXT("PROPERTY_TYPE_UNSUPPORTED"),
            FString::Printf(TEXT("Property type '%s' is unsupported for JSON assignment."), *Property->GetClass()->GetName()),
            AssetPath, OperationIndex, Callsite);
        return false;

    TypeMismatch:
        OutError = MakeTypeError(TEXT("PROPERTY_TYPE_MISMATCH"),
            FString::Printf(TEXT("JSON value does not match property '%s' of type '%s'."), *Property->GetName(), *Property->GetCPPType()),
            AssetPath, OperationIndex, Callsite);
        return false;
    }

    TSharedPtr<FJsonValue> FBlueprintTypeSystem::GetPropertyValue(UObject* Owner, const FString& PropertyPath,
        FBlueprintOperationError& OutError, const FString& AssetPath, const int32 OperationIndex)
    {
        if (!RequireTypeGameThread(OutError, AssetPath, OperationIndex, TEXT("FBlueprintTypeSystem::GetPropertyValue"))) return nullptr;
        FProperty* Property = nullptr;
        void* Address = nullptr;
        if (!ResolvePropertyPath(Owner, PropertyPath, Property, Address, OutError, AssetPath, OperationIndex))
        {
            return nullptr;
        }
        return PropertyValueToJson(Property, Address, OutError, AssetPath, OperationIndex);
    }

    TSharedPtr<FJsonValue> FBlueprintTypeSystem::PropertyValueToJson(const FProperty* Property, const void* ValueAddress,
        FBlueprintOperationError& OutError, const FString& AssetPath, const int32 OperationIndex)
    {
        if (!RequireTypeGameThread(OutError, AssetPath, OperationIndex, TEXT("FBlueprintTypeSystem::PropertyValueToJson"))) return nullptr;
        const FString Callsite = TEXT("FBlueprintTypeSystem::PropertyValueToJson");
        if (const FBoolProperty* Typed = CastField<FBoolProperty>(Property)) return MakeShared<FJsonValueBoolean>(Typed->GetPropertyValue(ValueAddress));
        if (const FEnumProperty* Typed = CastField<FEnumProperty>(Property)) return MakeShared<FJsonValueString>(Typed->GetEnum()->GetNameStringByValue(Typed->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValueAddress)));
        if (const FNumericProperty* Typed = CastField<FNumericProperty>(Property))
        {
            if (Typed->IsEnum()) return MakeShared<FJsonValueString>(Typed->GetIntPropertyEnum()->GetNameStringByValue(Typed->GetSignedIntPropertyValue(ValueAddress)));
            return NumberToJson(Typed->IsInteger() ? static_cast<double>(Typed->GetSignedIntPropertyValue(ValueAddress)) : Typed->GetFloatingPointPropertyValue(ValueAddress));
        }
        if (const FStrProperty* Typed = CastField<FStrProperty>(Property)) return MakeShared<FJsonValueString>(Typed->GetPropertyValue(ValueAddress));
        if (const FNameProperty* Typed = CastField<FNameProperty>(Property)) return MakeShared<FJsonValueString>(Typed->GetPropertyValue(ValueAddress).ToString());
        if (const FTextProperty* Typed = CastField<FTextProperty>(Property)) return MakeShared<FJsonValueString>(Typed->GetPropertyValue(ValueAddress).ToString());
        if (const FObjectPropertyBase* Typed = CastField<FObjectPropertyBase>(Property))
        {
            UObject* Object = Typed->GetObjectPropertyValue(ValueAddress);
            if (Object != nullptr)
            {
                return MakeShared<FJsonValueString>(Object->GetPathName());
            }
            return MakeShared<FJsonValueNull>();
        }
        if (const FSoftObjectProperty* Typed = CastField<FSoftObjectProperty>(Property)) return MakeShared<FJsonValueString>(Typed->GetPropertyValue(ValueAddress).ToSoftObjectPath().ToString());
        if (const FStructProperty* Typed = CastField<FStructProperty>(Property))
        {
            TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
            for (TFieldIterator<FProperty> It(Typed->Struct); It; ++It)
            {
                TSharedPtr<FJsonValue> Child = PropertyValueToJson(*It, It->ContainerPtrToValuePtr<void>(ValueAddress), OutError, AssetPath, OperationIndex);
                if (!Child.IsValid()) return nullptr;
                Json->SetField(It->GetName(), Child);
            }
            return MakeShared<FJsonValueObject>(Json);
        }
        if (const FArrayProperty* Typed = CastField<FArrayProperty>(Property))
        {
            FScriptArrayHelper Helper(Typed, ValueAddress);
            TArray<TSharedPtr<FJsonValue>> Json;
            for (int32 Index = 0; Index < Helper.Num(); ++Index)
            {
                TSharedPtr<FJsonValue> Item = PropertyValueToJson(Typed->Inner, Helper.GetRawPtr(Index), OutError, AssetPath, OperationIndex);
                if (!Item.IsValid()) return nullptr;
                Json.Add(Item);
            }
            return MakeShared<FJsonValueArray>(Json);
        }
        if (const FSetProperty* Typed = CastField<FSetProperty>(Property))
        {
            FScriptSetHelper Helper(Typed, ValueAddress);
            TArray<TSharedPtr<FJsonValue>> Json;
            for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
            {
                if (!Helper.IsValidIndex(Index)) continue;
                TSharedPtr<FJsonValue> Item = PropertyValueToJson(Typed->ElementProp, Helper.GetElementPtr(Index), OutError, AssetPath, OperationIndex);
                if (!Item.IsValid()) return nullptr;
                Json.Add(Item);
            }
            return MakeShared<FJsonValueArray>(Json);
        }
        if (const FMapProperty* Typed = CastField<FMapProperty>(Property))
        {
            FScriptMapHelper Helper(Typed, ValueAddress);
            TArray<TSharedPtr<FJsonValue>> Json;
            for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
            {
                if (!Helper.IsValidIndex(Index)) continue;
                TSharedPtr<FJsonValue> Key = PropertyValueToJson(Typed->KeyProp, Helper.GetKeyPtr(Index), OutError, AssetPath, OperationIndex);
                TSharedPtr<FJsonValue> MapValue = PropertyValueToJson(Typed->ValueProp, Helper.GetValuePtr(Index), OutError, AssetPath, OperationIndex);
                if (!Key.IsValid() || !MapValue.IsValid()) return nullptr;
                TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetField(TEXT("key"), Key);
                Entry->SetField(TEXT("value"), MapValue);
                Json.Add(MakeShared<FJsonValueObject>(Entry));
            }
            return MakeShared<FJsonValueArray>(Json);
        }
        OutError = MakeTypeError(TEXT("PROPERTY_TYPE_UNSUPPORTED"), FString::Printf(TEXT("Property type '%s' cannot be exported to JSON."), *Property->GetClass()->GetName()),
            AssetPath, OperationIndex, Callsite);
        return nullptr;
    }

    bool FBlueprintTypeSystem::ApplyVariableMetadata(UBlueprint* Blueprint, const FName VariableName,
        const FBlueprintVariableDefinition& Definition, FBlueprintOperationError& OutError, const int32 OperationIndex)
    {
        FBPVariableDescription* Variable = Blueprint->NewVariables.FindByPredicate([VariableName](const FBPVariableDescription& Candidate)
        {
            return Candidate.VarName == VariableName;
        });
        if (Variable == nullptr)
        {
            OutError = MakeTypeError(TEXT("VARIABLE_NOT_FOUND"), FString::Printf(TEXT("Variable '%s' was not found."), *VariableName.ToString()),
                Blueprint->GetPathName(), OperationIndex, TEXT("FBlueprintTypeSystem::ApplyVariableMetadata"));
            return false;
        }
        Variable->Category = FText::FromString(Definition.Category);
        Variable->FriendlyName = VariableName.ToString();
        Variable->PropertyFlags |= CPF_BlueprintVisible;
        Variable->PropertyFlags = Definition.bInstanceEditable ? (Variable->PropertyFlags | CPF_Edit) : (Variable->PropertyFlags & ~CPF_Edit);
        Variable->PropertyFlags = Definition.bExposeOnSpawn ? (Variable->PropertyFlags | CPF_ExposeOnSpawn) : (Variable->PropertyFlags & ~CPF_ExposeOnSpawn);
        Variable->PropertyFlags = Definition.bSaveGame ? (Variable->PropertyFlags | CPF_SaveGame) : (Variable->PropertyFlags & ~CPF_SaveGame);
        Variable->PropertyFlags = Definition.bAdvancedDisplay ? (Variable->PropertyFlags | CPF_AdvancedDisplay) : (Variable->PropertyFlags & ~CPF_AdvancedDisplay);
        Variable->PropertyFlags = Definition.bTransient ? (Variable->PropertyFlags | CPF_Transient) : (Variable->PropertyFlags & ~CPF_Transient);
        Variable->PropertyFlags = Definition.bReplicated ? (Variable->PropertyFlags | CPF_Net) : (Variable->PropertyFlags & ~CPF_Net);
        Variable->PropertyFlags = Definition.bRepNotify ? (Variable->PropertyFlags | CPF_RepNotify | CPF_Net) : (Variable->PropertyFlags & ~CPF_RepNotify);
        Variable->RepNotifyFunc = Definition.bRepNotify ? Definition.RepNotifyFunction : NAME_None;
        FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, VariableName, nullptr,
            FBlueprintMetadata::MD_Private, Definition.bPrivate ? TEXT("true") : TEXT("false"));
        if (!Definition.Tooltip.IsEmpty())
        {
            FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, VariableName, nullptr, FBlueprintMetadata::MD_Tooltip, Definition.Tooltip);
        }
        for (const TPair<FName, FString>& Meta : Definition.Metadata)
        {
            FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, VariableName, nullptr, Meta.Key, Meta.Value);
        }
        return true;
    }

    FBlueprintOperationResult FBlueprintTypeSystem::AddVariable(UBlueprint* Blueprint,
        const FBlueprintVariableDefinition& Definition, const int32 OperationIndex)
    {
        if (!IsInGameThread()) return TypeWrongThread(FString(), OperationIndex, TEXT("FBlueprintTypeSystem::AddVariable"));
        if (Blueprint == nullptr || Definition.Name.IsNone())
        {
            return FBlueprintOperationResult::Failure(TEXT("VARIABLE_INVALID"), TEXT("Blueprint and variable name are required."),
                Blueprint ? Blueprint->GetPathName() : FString(), OperationIndex, TEXT("FBlueprintTypeSystem::AddVariable"));
        }
        if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, Definition.Name) != INDEX_NONE)
        {
            return FBlueprintOperationResult::Failure(TEXT("VARIABLE_ALREADY_EXISTS"), FString::Printf(TEXT("Variable '%s' already exists."), *Definition.Name.ToString()),
                Blueprint->GetPathName(), OperationIndex, TEXT("FBlueprintTypeSystem::AddVariable"));
        }
        FString DefaultText;
        if (!JsonToDefaultText(Definition.DefaultValue, DefaultText, &Definition.Type))
        {
            return FBlueprintOperationResult::Failure(TEXT("VARIABLE_DEFAULT_INVALID"), TEXT("Variable default cannot be represented as Blueprint text."),
                Blueprint->GetPathName(), OperationIndex, TEXT("FBlueprintTypeSystem::AddVariable"));
        }
        Blueprint->Modify();
        if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, Definition.Name, Definition.Type, DefaultText))
        {
            return FBlueprintOperationResult::Failure(TEXT("VARIABLE_ADD_FAILED"), FString::Printf(TEXT("UE rejected variable '%s'."), *Definition.Name.ToString()),
                Blueprint->GetPathName(), OperationIndex, TEXT("FBlueprintEditorUtils::AddMemberVariable"));
        }
        FBlueprintOperationError Error;
        if (!ApplyVariableMetadata(Blueprint, Definition.Name, Definition, Error, OperationIndex))
        {
            FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, Definition.Name);
            FBlueprintOperationResult Result;
            Result.Error = Error;
            return Result;
        }
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        return FBlueprintOperationResult::Success({Blueprint->GetPathName()});
    }

    FBlueprintOperationResult FBlueprintTypeSystem::UpdateVariable(UBlueprint* Blueprint, const FName VariableName,
        const FBlueprintVariableDefinition& Definition, const int32 OperationIndex)
    {
        if (!IsInGameThread()) return TypeWrongThread(FString(), OperationIndex, TEXT("FBlueprintTypeSystem::UpdateVariable"));
        if (Blueprint == nullptr || FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VariableName) == INDEX_NONE)
        {
            return FBlueprintOperationResult::Failure(TEXT("VARIABLE_NOT_FOUND"), FString::Printf(TEXT("Variable '%s' was not found."), *VariableName.ToString()),
                Blueprint ? Blueprint->GetPathName() : FString(), OperationIndex, TEXT("FBlueprintTypeSystem::UpdateVariable"));
        }
        if (Definition.Name.IsNone() || (Definition.Name != VariableName
            && FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, Definition.Name) != INDEX_NONE))
        {
            return FBlueprintOperationResult::Failure(TEXT("VARIABLE_NAME_INVALID"),
                TEXT("The updated variable name is empty or already used."), Blueprint->GetPathName(), OperationIndex,
                TEXT("FBlueprintTypeSystem::UpdateVariable"));
        }
        FString DefaultText;
        if (!JsonToDefaultText(Definition.DefaultValue, DefaultText, &Definition.Type))
        {
            return FBlueprintOperationResult::Failure(TEXT("VARIABLE_DEFAULT_INVALID"), TEXT("Variable default cannot be represented as Blueprint text."),
                Blueprint->GetPathName(), OperationIndex, TEXT("FBlueprintTypeSystem::UpdateVariable"));
        }
        Blueprint->Modify();
        FBlueprintEditorUtils::ChangeMemberVariableType(Blueprint, VariableName, Definition.Type);
        FName UpdatedName = VariableName;
        if (Definition.Name != VariableName)
        {
            FBlueprintEditorUtils::RenameMemberVariable(Blueprint, VariableName, Definition.Name);
            UpdatedName = Definition.Name;
        }
        const int32 VariableIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, UpdatedName);
        if (VariableIndex == INDEX_NONE || Blueprint->NewVariables[VariableIndex].VarType != Definition.Type)
        {
            return FBlueprintOperationResult::Failure(TEXT("VARIABLE_UPDATE_REJECTED"),
                TEXT("UE4.27 rejected the variable name or type update."), Blueprint->GetPathName(), OperationIndex,
                TEXT("FBlueprintEditorUtils::ChangeMemberVariableType/RenameMemberVariable"));
        }
        FBlueprintOperationError Error;
        if (!ApplyVariableMetadata(Blueprint, UpdatedName, Definition, Error, OperationIndex))
        {
            FBlueprintOperationResult Result;
            Result.Error = Error;
            return Result;
        }
        Blueprint->NewVariables[VariableIndex].DefaultValue = DefaultText;
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        return FBlueprintOperationResult::Success({Blueprint->GetPathName()});
    }

    FBlueprintOperationResult FBlueprintTypeSystem::RemoveVariable(UBlueprint* Blueprint, const FName VariableName,
        const int32 OperationIndex)
    {
        if (!IsInGameThread()) return TypeWrongThread(FString(), OperationIndex, TEXT("FBlueprintTypeSystem::RemoveVariable"));
        if (Blueprint == nullptr || FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VariableName) == INDEX_NONE)
        {
            return FBlueprintOperationResult::Failure(TEXT("VARIABLE_NOT_FOUND"), FString::Printf(TEXT("Variable '%s' was not found."), *VariableName.ToString()),
                Blueprint ? Blueprint->GetPathName() : FString(), OperationIndex, TEXT("FBlueprintTypeSystem::RemoveVariable"));
        }
        Blueprint->Modify();
        FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, VariableName);
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        return FBlueprintOperationResult::Success({Blueprint->GetPathName()});
    }
}
