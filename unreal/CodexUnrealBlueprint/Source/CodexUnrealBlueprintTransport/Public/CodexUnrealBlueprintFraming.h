#pragma once

#include "CoreMinimal.h"

namespace CodexUnrealBlueprint
{
    enum class EFrameDecodeResult : uint8
    {
        NeedMoreData,
        Complete,
        Invalid
    };

    class CODEXUNREALBLUEPRINTTRANSPORT_API FLengthPrefixedJsonFraming
    {
    public:
        static constexpr uint32 HeaderSize = 4;
        static constexpr uint32 MaxPayloadSize = 8u * 1024u * 1024u;

        static bool Encode(const FString& Json, TArray<uint8>& OutFrame, FString& OutError);
        static EFrameDecodeResult TryDecode(TArray<uint8>& InOutBuffer, FString& OutJson, FString& OutError);
    };
}
