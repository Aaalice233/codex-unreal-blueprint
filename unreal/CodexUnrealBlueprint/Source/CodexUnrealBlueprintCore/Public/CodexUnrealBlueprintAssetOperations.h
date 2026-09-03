#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraphPin.h"
#include "CodexUnrealBlueprintOperationResult.h"

class UBlueprint;
class UClass;
class UObject;
class UObjectRedirector;
class UUserDefinedEnum;
class UUserDefinedStruct;
class UWorld;

namespace CodexUnrealBlueprint
{
    enum class EBlueprintAssetKind : uint8
    {
        Blueprint,
        Interface,
        FunctionLibrary,
        MacroLibrary,
        UserDefinedStruct,
        UserDefinedEnum
    };

    class CODEXUNREALBLUEPRINTCORE_API FBlueprintAssetOperations
    {
    public:
        static FBlueprintOperationResult Create(const FString& PackagePath, EBlueprintAssetKind Kind,
            UClass* ParentClass, int32 OperationIndex = INDEX_NONE);
        static FBlueprintOperationResult GetOrCreateLevelBlueprint(UWorld* World, int32 OperationIndex = INDEX_NONE);
        static FBlueprintOperationResult Duplicate(UObject* SourceAsset, const FString& DestinationPackagePath,
            int32 OperationIndex = INDEX_NONE);
        static FBlueprintOperationResult RenameOrMove(UObject* Asset, const FString& DestinationPackagePath,
            int32 OperationIndex = INDEX_NONE);
        static FBlueprintOperationResult Delete(UObject* Asset, int32 OperationIndex = INDEX_NONE);
        static FBlueprintOperationResult ResetParent(UBlueprint* Blueprint, UClass* NewParentClass,
            int32 OperationIndex = INDEX_NONE);
        static FBlueprintOperationResult AddInterface(UBlueprint* Blueprint, UClass* InterfaceClass,
            int32 OperationIndex = INDEX_NONE);
        static FBlueprintOperationResult RemoveInterface(UBlueprint* Blueprint, UClass* InterfaceClass,
            bool bPreserveFunctions, int32 OperationIndex = INDEX_NONE);
        static FBlueprintOperationResult FixRedirector(UObjectRedirector* Redirector,
            int32 OperationIndex = INDEX_NONE);

        static FBlueprintOperationResult ReadClassDefaults(UBlueprint* Blueprint,
            const TArray<FString>& PropertyPaths, int32 OperationIndex = INDEX_NONE);
        static FBlueprintOperationResult SetClassDefault(UBlueprint* Blueprint, const FString& PropertyPath,
            const TSharedPtr<FJsonValue>& Value, int32 OperationIndex = INDEX_NONE);

        static FBlueprintOperationResult AddStructField(UUserDefinedStruct* Struct, const FString& DisplayName,
            const FEdGraphPinType& Type, const FString& DefaultValue, const FString& Tooltip,
            int32 OperationIndex = INDEX_NONE);
        static FBlueprintOperationResult UpdateStructField(UUserDefinedStruct* Struct, const FGuid& FieldGuid,
            const FString& DisplayName, const FEdGraphPinType& Type, const FString& DefaultValue,
            const FString& Tooltip, int32 OperationIndex = INDEX_NONE);
        static FBlueprintOperationResult RemoveStructField(UUserDefinedStruct* Struct, const FGuid& FieldGuid,
            int32 OperationIndex = INDEX_NONE);

        static FBlueprintOperationResult AddEnumValue(UUserDefinedEnum* Enum, const FString& DisplayName,
            int32 OperationIndex = INDEX_NONE);
        static FBlueprintOperationResult RenameEnumValue(UUserDefinedEnum* Enum, int32 ValueIndex,
            const FString& DisplayName, int32 OperationIndex = INDEX_NONE);
        static FBlueprintOperationResult RemoveEnumValue(UUserDefinedEnum* Enum, int32 ValueIndex,
            int32 OperationIndex = INDEX_NONE);
        static FBlueprintOperationResult SetEnumBitflags(UUserDefinedEnum* Enum, bool bBitflags,
            int32 OperationIndex = INDEX_NONE);

    private:
        static bool SplitPackagePath(const FString& PackagePath, FString& OutPackageName, FString& OutAssetName,
            FBlueprintOperationError& OutError, int32 OperationIndex);
    };
}
