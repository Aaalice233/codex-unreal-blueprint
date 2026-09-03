#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraphPin.h"
#include "CodexUnrealBlueprintOperationResult.h"

class FProperty;
class UBlueprint;
class UObject;
class UStruct;

namespace CodexUnrealBlueprint
{
    struct CODEXUNREALBLUEPRINTCORE_API FBlueprintVariableDefinition
    {
        FName Name;
        FEdGraphPinType Type;
        TSharedPtr<FJsonValue> DefaultValue;
        FString Category;
        FString Tooltip;
        TMap<FName, FString> Metadata;
        bool bInstanceEditable = false;
        bool bExposeOnSpawn = false;
        bool bPrivate = false;
        bool bSaveGame = false;
        bool bAdvancedDisplay = false;
        bool bTransient = false;
        bool bReplicated = false;
        bool bRepNotify = false;
        FName RepNotifyFunction;
    };

    class CODEXUNREALBLUEPRINTCORE_API FBlueprintTypeSystem
    {
    public:
        static bool ParsePinType(const TSharedPtr<FJsonObject>& Json, FEdGraphPinType& OutType,
            FBlueprintOperationError& OutError, const FString& AssetPath = FString(), int32 OperationIndex = INDEX_NONE);
        static TSharedRef<FJsonObject> PinTypeToJson(const FEdGraphPinType& Type);

        static bool SetPropertyValue(UObject* Owner, const FString& PropertyPath, const TSharedPtr<FJsonValue>& Value,
            FBlueprintOperationError& OutError, const FString& AssetPath = FString(), int32 OperationIndex = INDEX_NONE);
        static bool SetPropertyValue(FProperty* Property, void* ValueAddress, const TSharedPtr<FJsonValue>& Value,
            FBlueprintOperationError& OutError, const FString& AssetPath = FString(), int32 OperationIndex = INDEX_NONE);
        static TSharedPtr<FJsonValue> GetPropertyValue(UObject* Owner, const FString& PropertyPath,
            FBlueprintOperationError& OutError, const FString& AssetPath = FString(), int32 OperationIndex = INDEX_NONE);
        static TSharedPtr<FJsonValue> PropertyValueToJson(const FProperty* Property, const void* ValueAddress,
            FBlueprintOperationError& OutError, const FString& AssetPath = FString(), int32 OperationIndex = INDEX_NONE);

        static FBlueprintOperationResult AddVariable(UBlueprint* Blueprint, const FBlueprintVariableDefinition& Definition,
            int32 OperationIndex = INDEX_NONE);
        static FBlueprintOperationResult UpdateVariable(UBlueprint* Blueprint, const FName VariableName,
            const FBlueprintVariableDefinition& Definition, int32 OperationIndex = INDEX_NONE);
        static FBlueprintOperationResult RemoveVariable(UBlueprint* Blueprint, const FName VariableName,
            int32 OperationIndex = INDEX_NONE);

    private:
        static UObject* ResolveTypeObject(const FString& ObjectPath, UClass* RequiredClass,
            FBlueprintOperationError& OutError, const FString& AssetPath, int32 OperationIndex);
        static bool ResolvePropertyPath(UObject* Owner, const FString& PropertyPath, FProperty*& OutProperty,
            void*& OutAddress, FBlueprintOperationError& OutError, const FString& AssetPath, int32 OperationIndex);
        static bool ApplyVariableMetadata(UBlueprint* Blueprint, const FName VariableName,
            const FBlueprintVariableDefinition& Definition, FBlueprintOperationError& OutError, int32 OperationIndex);
    };
}
