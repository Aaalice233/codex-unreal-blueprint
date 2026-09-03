#pragma once

#include "CoreMinimal.h"
#include "PiUnrealBlueprintProtocol.h"

namespace PiUnrealBlueprint
{
    class PIUNREALBLUEPRINTCORE_API FCoreService
    {
    public:
        static FCoreService& Get();

        using FResponseCallback = TFunction<void(FProtocolResponse&&)>;

        FProtocolResponse Dispatch(const FProtocolRequest& Request) const;
        void DispatchAsync(const FProtocolRequest& Request, FResponseCallback Callback) const;
        FProtocolResponse MakeNotImplemented(const FString& RequestId, const FString& Method) const;

    private:
        FProtocolResponse GetStatus(const FProtocolRequest& Request) const;
        FProtocolResponse Doctor(const FProtocolRequest& Request) const;
        FProtocolResponse Search(const FProtocolRequest& Request) const;
        FProtocolResponse Capabilities(const FProtocolRequest& Request) const;
        FProtocolResponse Inspect(const FProtocolRequest& Request) const;
        FProtocolResponse Validate(const FProtocolRequest& Request) const;
        FProtocolResponse Apply(const FProtocolRequest& Request) const;
        FProtocolResponse GetJob(const FProtocolRequest& Request) const;
        FProtocolResponse CancelJob(const FProtocolRequest& Request) const;
        FProtocolResponse Verify(const FProtocolRequest& Request) const;
        FProtocolResponse GetRequestJournal(const FProtocolRequest& Request) const;
    };
}
