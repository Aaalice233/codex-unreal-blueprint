#pragma once

#include "CoreMinimal.h"
#include "CodexUnrealBlueprintProtocol.h"

namespace CodexUnrealBlueprint
{
    struct CODEXUNREALBLUEPRINTTRANSPORT_API FTransportServerConfig
    {
        FString SessionDirectoryOverride;
        FString UprojectOverride;
        bool bWriteSessionDescriptor = true;
    };

    class CODEXUNREALBLUEPRINTTRANSPORT_API FTransportServer
    {
    public:
        FTransportServer();
        ~FTransportServer();

        bool Start(const FTransportServerConfig& Config, FProtocolError& OutError);
        void Stop();

        EServiceState GetState() const;
        int32 GetPort() const;
        int32 GetConnectionCount() const;
        FString GetEditorSessionId() const;
        FString GetAuthToken() const;
        FString GetSessionDescriptorPath() const;

    private:
        class FImpl;
        TUniquePtr<FImpl> Impl;
    };
}
