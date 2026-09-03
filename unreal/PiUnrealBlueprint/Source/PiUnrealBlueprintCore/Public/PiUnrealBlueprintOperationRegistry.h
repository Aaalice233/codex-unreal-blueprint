#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "PiUnrealBlueprintProtocol.h"
#include "PiUnrealBlueprintWritePipeline.h"

namespace PiUnrealBlueprint
{
    struct PIUNREALBLUEPRINTCORE_API FOperationDefinition
    {
        FString Name;
        FString Domain;
        FString Description;
        TSharedRef<FJsonObject> Schema;
        TSharedRef<FJsonObject> Example;
    };

    /** The single authoritative schema and dispatch registry for public Blueprint operations. */
    class PIUNREALBLUEPRINTCORE_API FOperationRegistry
    {
    public:
        static FOperationRegistry& Get();

        const FOperationDefinition* Find(const FString& Name) const;
        TSharedRef<FJsonObject> GetCapabilities(const FString& Domain,
            const TArray<FString>& OperationNames, FProtocolError& OutError) const;
        TSharedRef<FJsonObject> Search(const FString& Query, const FString& Domain,
            int32 Offset, int32 Limit) const;
        bool Validate(const TArray<TSharedRef<FJsonObject>>& Operations,
            FPreflightRequest& OutPreflight, TSharedRef<FJsonObject>& OutResult,
            FProtocolError& OutError) const;
        bool BuildWriteRequest(const FString& RequestId,
            const TArray<TSharedRef<FJsonObject>>& Operations,
            const TMap<FString, FString>& ExpectedStateHashes,
            FWritePipelineRequest& OutRequest, FProtocolError& OutError) const;

    private:
        FOperationRegistry();
        TMap<FString, FOperationDefinition> Definitions;
    };
}
