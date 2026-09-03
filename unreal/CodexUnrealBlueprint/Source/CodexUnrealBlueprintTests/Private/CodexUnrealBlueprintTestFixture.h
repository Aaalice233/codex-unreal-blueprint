#pragma once

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "CodexUnrealBlueprintAssetOperations.h"

class UAnimBlueprint;
class UBlueprint;
class UObject;
class USkeleton;
class UUserDefinedEnum;
class UUserDefinedStruct;
class UWidgetBlueprint;
class UWorld;

namespace CodexUnrealBlueprintTests
{
    class FScopedFixture
    {
    public:
        explicit FScopedFixture(const FString& SuiteName);
        ~FScopedFixture();

        const FString& GetRunId() const { return RunId; }
        const FString& GetRoot() const { return Root; }
        FString Package(const FString& Leaf) const;

        UBlueprint* CreateBlueprint(const FString& Leaf, UClass* ParentClass = nullptr);
        UUserDefinedStruct* CreateStruct(const FString& Leaf);
        UUserDefinedEnum* CreateEnum(const FString& Leaf);
        UWidgetBlueprint* CreateWidgetBlueprint(const FString& Leaf);
        UAnimBlueprint* CreateAnimBlueprint(const FString& Leaf);
        UWorld* CreateWorld(const FString& Leaf);

        bool Save(UObject* Asset, FString& OutFilename);
        bool UnloadAndReload(const FString& PackageName, UObject*& OutAsset);
        bool Cleanup(FString* OutError = nullptr);

        static TSharedRef<FJsonObject> Operation(const FString& Name, const TCHAR* FieldName = nullptr,
            const FString& FieldValue = FString());
        static FString ObjectPath(const FString& PackageName);
        static USkeleton* FindAnySkeleton();

    private:
        UObject* Track(UObject* Asset);

        FString RunId;
        FString Root;
        TArray<TWeakObjectPtr<UObject>> Assets;
        bool bCleaned = false;
    };

    class FScopedDirectory
    {
    public:
        explicit FScopedDirectory(const FString& InPath);
        ~FScopedDirectory();

        const FString& GetPath() const { return Path; }
        bool Cleanup(FString* OutError = nullptr);

    private:
        FString Path;
        bool bCleaned = false;
    };
}

#endif
