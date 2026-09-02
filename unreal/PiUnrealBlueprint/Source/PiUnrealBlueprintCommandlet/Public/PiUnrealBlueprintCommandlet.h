#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "PiUnrealBlueprintCommandlet.generated.h"

UCLASS()
class PIUNREALBLUEPRINTCOMMANDLET_API UPiUnrealBlueprintCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:
    UPiUnrealBlueprintCommandlet(const FObjectInitializer& ObjectInitializer);
    virtual int32 Main(const FString& Params) override;

private:
    bool WriteResponse(const FString& ResultPath, const FString& Json) const;
};
