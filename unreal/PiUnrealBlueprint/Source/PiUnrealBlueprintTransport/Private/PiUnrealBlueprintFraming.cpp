#include "PiUnrealBlueprintFraming.h"

#include "Containers/StringConv.h"

namespace PiUnrealBlueprint
{
    namespace
    {
        bool IsValidUtf8(const uint8* Data, const uint32 Length)
        {
            uint32 Index = 0;
            while (Index < Length)
            {
                const uint8 First = Data[Index++];
                if (First <= 0x7f)
                {
                    continue;
                }
                if (First >= 0xc2 && First <= 0xdf)
                {
                    if (Index >= Length || (Data[Index++] & 0xc0) != 0x80) return false;
                    continue;
                }
                if (First >= 0xe0 && First <= 0xef)
                {
                    if (Index + 1 >= Length) return false;
                    const uint8 Second = Data[Index++];
                    const uint8 Third = Data[Index++];
                    if ((Third & 0xc0) != 0x80) return false;
                    if (First == 0xe0 && (Second < 0xa0 || Second > 0xbf)) return false;
                    if (First == 0xed && (Second < 0x80 || Second > 0x9f)) return false;
                    if (First != 0xe0 && First != 0xed && (Second & 0xc0) != 0x80) return false;
                    continue;
                }
                if (First >= 0xf0 && First <= 0xf4)
                {
                    if (Index + 2 >= Length) return false;
                    const uint8 Second = Data[Index++];
                    const uint8 Third = Data[Index++];
                    const uint8 Fourth = Data[Index++];
                    if ((Third & 0xc0) != 0x80 || (Fourth & 0xc0) != 0x80) return false;
                    if (First == 0xf0 && (Second < 0x90 || Second > 0xbf)) return false;
                    if (First == 0xf4 && (Second < 0x80 || Second > 0x8f)) return false;
                    if (First != 0xf0 && First != 0xf4 && (Second & 0xc0) != 0x80) return false;
                    continue;
                }
                return false;
            }
            return true;
        }
    }

    bool FLengthPrefixedJsonFraming::Encode(const FString& Json, TArray<uint8>& OutFrame, FString& OutError)
    {
        const FTCHARToUTF8 Utf8(*Json);
        const int32 PayloadLength = Utf8.Length();
        if (PayloadLength <= 0)
        {
            OutError = TEXT("Cannot encode an empty JSON payload.");
            return false;
        }
        if (static_cast<uint32>(PayloadLength) > MaxPayloadSize)
        {
            OutError = FString::Printf(TEXT("JSON payload exceeds the %u byte frame limit."), MaxPayloadSize);
            return false;
        }

        const uint32 Size = static_cast<uint32>(PayloadLength);
        OutFrame.Reset(HeaderSize + PayloadLength);
        OutFrame.Add(static_cast<uint8>((Size >> 24) & 0xff));
        OutFrame.Add(static_cast<uint8>((Size >> 16) & 0xff));
        OutFrame.Add(static_cast<uint8>((Size >> 8) & 0xff));
        OutFrame.Add(static_cast<uint8>(Size & 0xff));
        OutFrame.Append(reinterpret_cast<const uint8*>(Utf8.Get()), PayloadLength);
        OutError.Reset();
        return true;
    }

    EFrameDecodeResult FLengthPrefixedJsonFraming::TryDecode(TArray<uint8>& InOutBuffer, FString& OutJson, FString& OutError)
    {
        if (InOutBuffer.Num() < static_cast<int32>(HeaderSize))
        {
            return EFrameDecodeResult::NeedMoreData;
        }

        const uint32 Size = (static_cast<uint32>(InOutBuffer[0]) << 24)
            | (static_cast<uint32>(InOutBuffer[1]) << 16)
            | (static_cast<uint32>(InOutBuffer[2]) << 8)
            | static_cast<uint32>(InOutBuffer[3]);
        if (Size == 0 || Size > MaxPayloadSize)
        {
            OutError = FString::Printf(TEXT("Invalid JSON frame payload size: %u."), Size);
            return EFrameDecodeResult::Invalid;
        }
        if (InOutBuffer.Num() < static_cast<int32>(HeaderSize + Size))
        {
            return EFrameDecodeResult::NeedMoreData;
        }

        const uint8* Payload = InOutBuffer.GetData() + HeaderSize;
        if (!IsValidUtf8(Payload, Size))
        {
            OutError = TEXT("JSON frame payload is not valid UTF-8.");
            return EFrameDecodeResult::Invalid;
        }

        const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Payload), Size);
        OutJson = FString(Converted.Length(), Converted.Get());
        InOutBuffer.RemoveAt(0, HeaderSize + Size, false);
        OutError.Reset();
        return EFrameDecodeResult::Complete;
    }
}
