#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "PiUnrealBlueprintCommandlet.generated.h"

namespace PiUnrealBlueprint
{
    struct FProtocolResponse;

    // Commandlet 对 apply 保持进程存活，直到 Core Job 返回可检查的终态快照。
    PIUNREALBLUEPRINTCOMMANDLET_API void CompleteCommandletApply(
        const FString& Method, FProtocolResponse& Response);
}

UCLASS()
class PIUNREALBLUEPRINTCOMMANDLET_API UPiUnrealBlueprintCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:
    UPiUnrealBlueprintCommandlet(const FObjectInitializer& ObjectInitializer);
    virtual int32 Main(const FString& Params) override;

private:
    enum class EExitCode : int32
    {
        Success = 0,
        InvalidInvocationOrRequest = 2,
        RequestFailed = 3,
        ResultIoFailure = 4
    };

    bool WriteResponseAtomically(const FString& ResultPath, const FString& Json) const;
    static EExitCode ExitCodeForResponse(const PiUnrealBlueprint::FProtocolResponse& Response);
};
